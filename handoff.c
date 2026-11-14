/* SPDX-License-Identifier: MIT. Boot-services-only GPU display/PCI adapters.
 * One existing BGRX GOP mode is preserved; no native GOP mode switch occurs.
 * These RAM hooks cease to matter after ExitBootServices. No runtime/S3 hook.
 */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *boot_gop,boot_gop_original;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE boot_mode_original;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION boot_info;
static EFI_PCI_IO_PROTOCOL *boot_pci,boot_pci_original;
static BOOLEAN boot_prepared,boot_installed,boot_large;
static UINT64 boot_fb;
static UINT32 boot_row[8192];
static UINT64 boot_blt_calls,boot_blt_errors,boot_pci_calls;
static UINT8 boot_bar_template[2][sizeof(EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR)+2];
/* Protocol resource indices need not equal the physical BAR register slots.
 * This MSI PCI bus driver uses one shared compact array for Mem and attributes. */
static UINT8 boot_bar_index[3]; /* Physical BAR0, BAR1, BAR3, respectively. */
static BOOLEAN boot_bar_max_is_end[2];
static const char *boot_preflight_stage="display adapter check not reached";

static BOOLEAN boot_relocated_bar(UINT8 bar) {
    return bar<6 && (bar==boot_bar_index[1] || bar==boot_bar_index[2]);
}
static BOOLEAN boot_find_bars(void) {
    const UINT64 bases[]={T.old_bar0,MOVE_OLD_BAR1,MOVE_OLD_BAR3};
    const UINT64 lengths[]={T.len0,T.len1,T.len3};
    for(UINTN i=0;i<3;++i)boot_bar_index[i]=0xff;
    for(UINT8 bar=0;bar<6;++bar) {
        void *r=NULL;EFI_STATUS e=boot_pci_original.GetBarAttributes(boot_pci,bar,NULL,&r);
        hex("handoff.bar-index",bar);hex("handoff.bar-descriptor-status",e);
        if(EFI_ERROR(e)) {
            if(r)bs->FreePool(r);
            if(e==EFI_UNSUPPORTED || e==EFI_NOT_FOUND)continue;
            return FALSE;
        }
        if(!r)return FALSE;
        /* Some firmware (AMI on Intel boards) answers unused indices with
         * EFI_SUCCESS and an empty list: just the end tag. That is "no BAR". */
        if(((UINT8 *)r)[0]==0x79){bs->FreePool(r);continue;}
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=r;
        BOOLEAN ok=d->Desc==0x8a && d->Len==sizeof(*d)-3;
        if(ok)ok=((UINT8 *)r)[sizeof(*d)]==0x79 && d->AddrTranslationOffset==0;
        if(ok) {
            descriptors(r);
            if(d->ResType==0) {
                UINTN i=0;while(i<3 && d->AddrRangeMin!=bases[i])++i;
                ok=i<3 && boot_bar_index[i]==0xff && d->AddrLen==lengths[i] &&
                    d->AddrSpaceGranularity==(i?64:32) && (!i || (d->SpecificFlag&4));
                if(ok)ok=d->AddrRangeMax==lengths[i]-1 || d->AddrRangeMax==bases[i]+lengths[i]-1;
                if(ok) {
                    boot_bar_index[i]=bar;
                    if(i) {
                        memcpy(boot_bar_template[i-1],r,sizeof(boot_bar_template[i-1]));
                        boot_bar_max_is_end[i-1]=d->AddrRangeMax==bases[i]+lengths[i]-1;
                    }
                }
            } else ok=d->ResType==1; /* I/O-port resources keep their native methods. */
        }
        bs->FreePool(r);if(!ok)return FALSE;
    }
    for(UINTN i=0;i<3;++i)if(boot_bar_index[i]==0xff)return FALSE;
    hex("handoff.physical-bar0-index",boot_bar_index[0]);
    hex("handoff.physical-bar1-index",boot_bar_index[1]);
    hex("handoff.physical-bar3-index",boot_bar_index[2]);
    hex("handoff.bar1-max-is-end",boot_bar_max_is_end[0]);
    hex("handoff.bar3-max-is-end",boot_bar_max_is_end[1]);
    return TRUE;
}

static BOOLEAN boot_rect(UINTN x,UINTN y,UINTN w,UINTN h) {
    return w && h && x<boot_info.HorizontalResolution && y<boot_info.VerticalResolution &&
        w<=boot_info.HorizontalResolution-x && h<=boot_info.VerticalResolution-y;
}
static BOOLEAN boot_buffer(UINTN x,UINTN y,UINTN w,UINTN h,UINTN *delta,UINTN buffer) {
    UINTN max=~(UINTN)0;
    if(!buffer || !w || !h || x>max-w || x+w>max/4)return FALSE;
    if(!*delta){if(x || y)return FALSE;*delta=w*4;}
    if((*delta&3) || *delta<(x+w)*4 || y>max-(h-1))return FALSE;
    UINTN last=y+h-1;
    if(last>max/(*delta))return FALSE;
    UINTN bytes=(x+w)*4;
    return last*(*delta)<=max-bytes && last*(*delta)+bytes<=max-buffer;
}
static UINT64 boot_pixel(UINTN x,UINTN y) {
    return boot_fb+((UINT64)y*boot_info.PixelsPerScanLine+x)*4;
}
static EFI_STATUS boot_blt_body(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION op,UINTN sx,UINTN sy,UINTN dx,UINTN dy,UINTN w,UINTN h,UINTN delta) {
    if((UINT32)op>=EfiGraphicsOutputBltOperationMax || !w || !h || w>8192)return EFI_INVALID_PARAMETER;
    if((op==EfiBltVideoToBltBuffer || op==EfiBltVideoToVideo) && !boot_rect(sx,sy,w,h))return EFI_INVALID_PARAMETER;
    if(op!=EfiBltVideoToBltBuffer && !boot_rect(dx,dy,w,h))return EFI_INVALID_PARAMETER;
    if(op==EfiBltVideoFill && !buffer)return EFI_INVALID_PARAMETER;
    if(op==EfiBltVideoToBltBuffer && !boot_buffer(dx,dy,w,h,&delta,(UINTN)buffer))return EFI_INVALID_PARAMETER;
    if(op==EfiBltBufferToVideo && !boot_buffer(sx,sy,w,h,&delta,(UINTN)buffer))return EFI_INVALID_PARAMETER;
    for(UINTN row=0;row<h;++row) {
        UINTN r=(op==EfiBltVideoToVideo && dy>sy)?h-1-row:row;
        EFI_STATUS e;
        if(op==EfiBltVideoFill)
            e=gpu_root->Mem.Write(gpu_root,EfiPciWidthFillUint32,boot_pixel(dx,dy+r),w,buffer);
        else if(op==EfiBltVideoToBltBuffer)
            e=gpu_root->Mem.Read(gpu_root,EfiPciWidthUint32,boot_pixel(sx,sy+r),w,(UINT8 *)buffer+(dy+r)*delta+dx*4);
        else if(op==EfiBltBufferToVideo)
            e=gpu_root->Mem.Write(gpu_root,EfiPciWidthUint32,boot_pixel(dx,dy+r),w,(UINT8 *)buffer+(sy+r)*delta+sx*4);
        else {
            e=gpu_root->Mem.Read(gpu_root,EfiPciWidthUint32,boot_pixel(sx,sy+r),w,boot_row);
            if(!EFI_ERROR(e))e=gpu_root->Mem.Write(gpu_root,EfiPciWidthUint32,boot_pixel(dx,dy+r),w,boot_row);
        }
        if(EFI_ERROR(e))return e;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI boot_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION op,UINTN sx,UINTN sy,UINTN dx,UINTN dy,UINTN w,UINTN h,UINTN delta) {
    if(self!=boot_gop || !boot_prepared)return EFI_INVALID_PARAMETER;
    EFI_TPL old=bs->RaiseTPL(TPL_NOTIFY);++boot_blt_calls;
    EFI_STATUS e=boot_blt_body(buffer,op,sx,sy,dx,dy,w,h,delta);
    if(EFI_ERROR(e))++boot_blt_errors;bs->RestoreTPL(old);return e;
}
static EFI_STATUS EFIAPI boot_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,UINT32 mode,UINTN *size,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
    if(self!=boot_gop || !size || !info || mode || !boot_prepared)return EFI_INVALID_PARAMETER;
    *info=NULL;*size=0;
    EFI_STATUS e=bs->AllocatePool(EfiBootServicesData,sizeof(boot_info),(void **)info);
    if(EFI_ERROR(e))return e;
    **info=boot_info;*size=sizeof(boot_info);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI boot_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,UINT32 mode) {
    if(self!=boot_gop || !boot_prepared)return EFI_INVALID_PARAMETER;
    if(mode)return EFI_UNSUPPORTED;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black={0};
    return boot_blt(self,&black,EfiBltVideoFill,0,0,0,0,boot_info.HorizontalResolution,boot_info.VerticalResolution,0);
}
static BOOLEAN boot_bar_address(UINT8 bar,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT64 offset,UINTN count,UINT64 *address) {
    if((UINT32)width>=EfiPciIoWidthMaximum || !count)return FALSE;
    UINT64 length,base;
    if(bar==0xff){base=0;length=~(UINT64)0;}
    else if(bar==boot_bar_index[0]){base=T.old_bar0;length=T.len0;}
    else if(bar==boot_bar_index[1]){base=MOVE_NEW_BAR1;length=1ULL<<35;}
    else if(bar==boot_bar_index[2]){base=MOVE_NEW_BAR1+(1ULL<<35);length=T.len3;}
    else return FALSE;
    UINTN step=1ULL<<(width&3);
    UINT64 bytes;
    if(width>=EfiPciIoWidthFifoUint8 && width<=EfiPciIoWidthFifoUint64)bytes=step;
    else {if(count>~(UINT64)0/step)return FALSE;bytes=(UINT64)count*step;}
    if((offset&(step-1)) || offset>=length || bytes>length-offset)return FALSE;
    *address=base+offset;return TRUE;
}
static EFI_STATUS EFIAPI boot_pci_read(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,void *buffer) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || !boot_relocated_bar(bar))return boot_pci_original.Mem.Read(self,width,bar,offset,count,buffer);
    UINT64 a;if(!buffer || !boot_bar_address(bar,width,offset,count,&a))return EFI_INVALID_PARAMETER;
    ++boot_pci_calls;return gpu_root->Mem.Read(gpu_root,(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width,a,count,buffer);
}
static EFI_STATUS EFIAPI boot_pci_write(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,void *buffer) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || !boot_relocated_bar(bar))return boot_pci_original.Mem.Write(self,width,bar,offset,count,buffer);
    UINT64 a;if(!buffer || !boot_bar_address(bar,width,offset,count,&a))return EFI_INVALID_PARAMETER;
    ++boot_pci_calls;return gpu_root->Mem.Write(gpu_root,(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width,a,count,buffer);
}
static EFI_STATUS EFIAPI boot_poll(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINT64 mask,UINT64 value,UINT64 delay,UINT64 *result) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || !boot_relocated_bar(bar))return boot_pci_original.PollMem(self,width,bar,offset,mask,value,delay,result);
    UINT64 a;if(!result || (UINT32)width>EfiPciIoWidthUint64 || !boot_bar_address(bar,width,offset,1,&a))return EFI_INVALID_PARAMETER;
    ++boot_pci_calls;return gpu_root->PollMem(gpu_root,(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width,a,mask,value,delay,result);
}
static EFI_STATUS EFIAPI boot_copy(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 dbar,UINT64 dest,UINT8 sbar,UINT64 src,UINTN count) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || (!boot_relocated_bar(dbar) && !boot_relocated_bar(sbar)))
        return boot_pci_original.CopyMem(self,width,dbar,dest,sbar,src,count);
    UINT64 d,s;
    if((UINT32)width>EfiPciIoWidthUint64 || !boot_bar_address(dbar,width,dest,count,&d) || !boot_bar_address(sbar,width,src,count,&s))return EFI_INVALID_PARAMETER;
    ++boot_pci_calls;return gpu_root->CopyMem(gpu_root,(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width,d,s,count);
}
static EFI_STATUS EFIAPI boot_get_bar(EFI_PCI_IO_PROTOCOL *self,UINT8 bar,UINT64 *supports,void **resources) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || !boot_relocated_bar(bar))return boot_pci_original.GetBarAttributes(self,bar,supports,resources);
    if(!supports && !resources)return EFI_INVALID_PARAMETER;
    if(supports)*supports=0; /* Changes to cache attributes are not advertised. */
    if(resources) {
        *resources=NULL;EFI_STATUS e=bs->AllocatePool(EfiBootServicesData,sizeof(boot_bar_template[0]),resources);
        if(EFI_ERROR(e))return e;
        UINTN i=bar==boot_bar_index[1]?0:1;
        memcpy(*resources,boot_bar_template[i],sizeof(boot_bar_template[0]));
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=*resources;
        d->AddrRangeMin=MOVE_NEW_BAR1+(i?(1ULL<<35):0);
        d->AddrLen=i?T.len3:(1ULL<<35);
        /* Preserve the native convention: MSI end address or EDK2 alignment. */
        d->AddrRangeMax=(boot_bar_max_is_end[i]?d->AddrRangeMin:0)+d->AddrLen-1;
    }
    ++boot_pci_calls;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI boot_set_bar(EFI_PCI_IO_PROTOCOL *self,UINT64 attr,UINT8 bar,UINT64 *offset,UINT64 *length) {
    if(self!=boot_pci)return EFI_INVALID_PARAMETER;
    if(!boot_large || !boot_relocated_bar(bar))return boot_pci_original.SetBarAttributes(self,attr,bar,offset,length);
    if(!offset || !length)return EFI_INVALID_PARAMETER;
    return EFI_UNSUPPORTED;
}
static BOOLEAN boot_writable(UINT64 a,UINTN size) {
    if(!mapped_image(a,size))return FALSE;
    for(UINTN i=0;i+memory_descriptor_size<=memory_map_size;i+=memory_descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *d=(void *)(memory_map+i);
        if(d->NumberOfPages>~(UINT64)0/4096)return FALSE;
        UINT64 bytes=d->NumberOfPages*4096;
        if(a>=d->PhysicalStart && a-d->PhysicalStart<bytes && size<=bytes-(a-d->PhysicalStart))
            return !(d->Attribute&(EFI_MEMORY_WP|EFI_MEMORY_RO|EFI_MEMORY_RP|EFI_MEMORY_RUNTIME));
    }
    return FALSE;
}
static BOOLEAN boot_prepare(void) {
    boot_prepared=FALSE;boot_installed=FALSE;boot_large=FALSE;
    boot_preflight_stage="GOP lookup";
    EFI_HANDLE *handles=NULL;UINTN n=0;
    EFI_STATUS located=bs->LocateHandleBuffer(ByProtocol,&gop_guid,NULL,&n,&handles);
    hex("handoff.gop-locate",located);hex("handoff.gop-count",n);
    if(EFI_ERROR(located))return FALSE;
    /* The GOP drawing into this GPU's BAR1; other adapters (iGPU) are left alone. */
    UINTN matches=0;boot_gop=NULL;
    for(UINTN i=0;i<n && i<32;++i) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
        if(!EFI_ERROR(bs->HandleProtocol(handles[i],&gop_guid,(void **)&g)) && g && g->Mode &&
           g->Mode->FrameBufferBase==T.fb_base){boot_gop=g;++matches;}
    }
    EFI_STATUS e=matches==1?EFI_SUCCESS:EFI_UNSUPPORTED;
    bs->FreePool(handles);if(EFI_ERROR(e) || !boot_gop || !boot_gop->Mode)return FALSE;
    boot_preflight_stage="GOP mode validation";
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=boot_gop->Mode;
    if(m->Mode>=m->MaxMode || !m->Info || m->SizeOfInfo<sizeof(boot_info) || m->FrameBufferBase!=T.fb_base ||
       m->Info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor || m->Info->Version ||
       !m->Info->HorizontalResolution || m->Info->HorizontalResolution>8192 ||
       !m->Info->VerticalResolution || m->Info->VerticalResolution>8192 ||
       m->Info->PixelsPerScanLine<m->Info->HorizontalResolution || m->Info->PixelsPerScanLine>8192 ||
       (UINT64)m->Info->PixelsPerScanLine*m->Info->VerticalResolution*4>m->FrameBufferSize ||
       m->FrameBufferSize>T.len1-(T.fb_base-MOVE_OLD_BAR1))return FALSE;
    boot_preflight_stage="GPU PCI protocol lookup";
    e=bs->HandleProtocol(gpu_handle,&pci_guid,(void **)&boot_pci);hex("handoff.pci-protocol",e);
    if(EFI_ERROR(e) || !boot_pci)return FALSE;
    if(!boot_pci->Mem.Read || !boot_pci->Mem.Write || !boot_pci->PollMem || !boot_pci->CopyMem ||
       !boot_pci->GetBarAttributes || !boot_pci->SetBarAttributes || !gpu_root->Mem.Write || !gpu_root->PollMem || !gpu_root->CopyMem)return FALSE;
    boot_gop_original=*boot_gop;boot_mode_original=*m;boot_info=*m->Info;
    boot_pci_original=*boot_pci;boot_fb=m->FrameBufferBase;
    boot_preflight_stage="PCI resource mapping";
    if(!boot_find_bars())return FALSE;
    boot_preflight_stage="firmware memory map";
    if(!capture_memory_map())return FALSE;
    boot_preflight_stage="writable protocol storage";
    BOOLEAN wg=boot_writable((UINTN)boot_gop,sizeof(*boot_gop)),wm=boot_writable((UINTN)m,sizeof(*m)),
        wp=boot_writable((UINTN)boot_pci,sizeof(*boot_pci));
    hex("handoff.writable-gop",wg);hex("handoff.writable-mode",wm);hex("handoff.writable-pci",wp);
    BOOLEAN writable=wg && wm && wp;
    bs->FreePool(memory_map);memory_map=NULL;memory_map_size=0;
    boot_prepared=writable;if(writable)boot_preflight_stage="display adapter ready";return writable;
}
static BOOLEAN boot_selftest(void) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL old[16],readback[16],paint={0x21,0x43,0x65,0};
    EFI_TPL tpl=bs->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS e=boot_blt_body(old,EfiBltVideoToBltBuffer,0,0,0,0,4,4,0);
    if(EFI_ERROR(e)){bs->RestoreTPL(tpl);return FALSE;}
    BOOLEAN ok=!EFI_ERROR(boot_blt_body(&paint,EfiBltVideoFill,0,0,0,0,4,4,0));
    if(!EFI_ERROR(boot_blt_body(readback,EfiBltVideoToBltBuffer,0,0,0,0,4,4,0))) {
        for(UINTN i=0;i<16;++i)if(!same(&readback[i],&paint,4))ok=FALSE;
    } else ok=FALSE;
    BOOLEAN restored=!EFI_ERROR(boot_blt_body(old,EfiBltBufferToVideo,0,0,0,0,4,4,0));
    if(EFI_ERROR(boot_blt_body(readback,EfiBltVideoToBltBuffer,0,0,0,0,4,4,0)) || !same(old,readback,sizeof(old)))restored=FALSE;
    bs->RestoreTPL(tpl);hex("handoff.framebuffer-selftest",ok && restored);return ok && restored;
}
static BOOLEAN boot_hooks_match(void) {
    return boot_installed && boot_gop->QueryMode==boot_query && boot_gop->SetMode==boot_set_mode && boot_gop->Blt==boot_blt &&
        boot_gop->Mode==boot_gop_original.Mode && boot_gop->Mode->MaxMode==1 && boot_gop->Mode->Mode==0 &&
        boot_gop->Mode->Info==&boot_info && boot_gop->Mode->SizeOfInfo==sizeof(boot_info) &&
        boot_gop->Mode->FrameBufferSize==boot_mode_original.FrameBufferSize &&
        boot_pci->Mem.Read==boot_pci_read && boot_pci->Mem.Write==boot_pci_write &&
        boot_pci->PollMem==boot_poll && boot_pci->CopyMem==boot_copy && boot_pci->GetBarAttributes==boot_get_bar && boot_pci->SetBarAttributes==boot_set_bar;
}
static BOOLEAN boot_install(void) {
    if(!boot_prepared || !same(boot_gop,&boot_gop_original,sizeof(*boot_gop)) ||
       !same(boot_gop->Mode,&boot_mode_original,sizeof(boot_mode_original)) ||
       !same(boot_gop->Mode->Info,&boot_info,sizeof(boot_info)) ||
       !same(boot_pci,&boot_pci_original,sizeof(*boot_pci)) || !boot_selftest())return FALSE;
    EFI_TPL tpl=bs->RaiseTPL(TPL_NOTIFY);
    boot_gop->QueryMode=boot_query;boot_gop->SetMode=boot_set_mode;boot_gop->Blt=boot_blt;
    boot_gop->Mode->MaxMode=1;boot_gop->Mode->Mode=0;boot_gop->Mode->Info=&boot_info;boot_gop->Mode->SizeOfInfo=sizeof(boot_info);
    boot_pci->Mem.Read=boot_pci_read;boot_pci->Mem.Write=boot_pci_write;boot_pci->PollMem=boot_poll;
    boot_pci->CopyMem=boot_copy;boot_pci->GetBarAttributes=boot_get_bar;boot_pci->SetBarAttributes=boot_set_bar;
    boot_installed=TRUE;bs->RestoreTPL(tpl);return boot_hooks_match();
}
static BOOLEAN boot_commit(UINT64 base) {
    /* Invoked with callbacks masked, after the 32 GiB mask and relocated
     * framebuffer sample passed, before the resize transaction can return. */
    if(base!=MOVE_NEW_BAR1 || !boot_hooks_match() || boot_large)return FALSE;
    UINT64 fb=base+(T.fb_base-MOVE_OLD_BAR1);
    boot_fb=fb;boot_gop->Mode->FrameBufferBase=fb;boot_large=TRUE;return TRUE;
}
static void boot_uninstall(void) {
    if(!boot_installed || boot_large)return;
    EFI_TPL tpl=bs->RaiseTPL(TPL_NOTIFY);
    *boot_gop_original.Mode=boot_mode_original;*boot_gop=boot_gop_original;
    boot_pci->Mem=boot_pci_original.Mem;boot_pci->PollMem=boot_pci_original.PollMem;
    boot_pci->CopyMem=boot_pci_original.CopyMem;boot_pci->GetBarAttributes=boot_pci_original.GetBarAttributes;
    boot_pci->SetBarAttributes=boot_pci_original.SetBarAttributes;
    boot_installed=FALSE;bs->RestoreTPL(tpl);
}
static BOOLEAN boot_gpu_expected(const UINT32 *after) {
    UINT32 expected[24];memcpy(expected,before_gpu,sizeof(expected));
    expected[5]=move_target[2];expected[6]=move_target[3];expected[7]=move_target[4];expected[8]=move_target[5];
    expected[14]=move_target[9];expected[18]=resize_supported;expected[19]=move_target[10];
    for(UINTN i=0;i<22;++i) {
        if(i==1){if((after[i]&0xffff)!=(expected[i]&0xffff))return FALSE;}
        else if(after[i]!=expected[i])return FALSE;
    }
    const UINT64 addresses[]={MOVE_BRIDGE+4,MOVE_BRIDGE+0x24,MOVE_BRIDGE+0x28,MOVE_BRIDGE+0x2c};
    const UINT32 values[]={move_target[1],move_target[6],move_target[7],move_target[8]};
    for(UINTN i=0;i<4;++i) {
        UINT32 v=0;if(EFI_ERROR(move_read(addresses[i],i==0,&v)) || v!=values[i])return FALSE;
    }
    return boot_large && boot_hooks_match() && boot_fb==MOVE_NEW_BAR1+(T.fb_base-MOVE_OLD_BAR1) && boot_gop->Mode->FrameBufferBase==boot_fb;
}
