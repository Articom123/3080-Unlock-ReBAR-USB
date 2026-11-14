/* SPDX-License-Identifier: MIT. Firmware metadata and selected loaded images. */
static EFI_GUID dxe_guid=DXE_SERVICES_TABLE_GUID,gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_DXE_SERVICES *ds;
struct blocked_range {UINT64 base,length;};
static struct blocked_range display_ranges[32];
static UINTN display_range_count;
static BOOLEAN display_capture_ok;

static BOOLEAN locate_dxe(void) {
    ds=NULL;
    for(UINTN i=0;i<st->NumberOfTableEntries && i<256;++i)
        if(same(&st->ConfigurationTable[i].VendorGuid,&dxe_guid,16)) {
            if(ds)return FALSE;ds=st->ConfigurationTable[i].VendorTable;
        }
    if(!ds || ds->Hdr.Signature!=DXE_SERVICES_SIGNATURE ||
       ds->Hdr.HeaderSize<sizeof(EFI_DXE_SERVICES) || ds->Hdr.HeaderSize>4096 ||
       !ds->GetMemorySpaceMap || !ds->AllocateMemorySpace || !ds->FreeMemorySpace)return FALSE;
    hex("dxe.revision",ds->Hdr.Revision);hex("dxe.header-size",ds->Hdr.HeaderSize);
    hex("dxe.header-crc32",ds->Hdr.CRC32);
    return TRUE;
}
static void capture_gop(void) {
    EFI_HANDLE *handles=NULL;UINTN count=0;display_range_count=0;display_capture_ok=TRUE;
    EFI_STATUS e=bs->LocateHandleBuffer(ByProtocol,&gop_guid,NULL,&count,&handles);
    hex("gop.locate",e);hex("gop.count",count);
    if(EFI_ERROR(e)){display_capture_ok=FALSE;return;}
    if(count>32){display_capture_ok=FALSE;bs->FreePool(handles);return;}
    for(UINTN i=0;i<count;++i) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
        e=bs->HandleProtocol(handles[i],&gop_guid,(void **)&gop);
        if(EFI_ERROR(e) || !gop || !gop->Mode){display_capture_ok=FALSE;continue;}
        EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=gop->Mode;
        emit("GOP");hex("gop.handle",(UINTN)handles[i]);dump_path(handles[i]);
        hex("gop.mode",m->Mode);hex("gop.framebuffer-base",m->FrameBufferBase);
        hex("gop.framebuffer-bytes",m->FrameBufferSize);
        if(m->Info && m->SizeOfInfo>=sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)) {
            hex("gop.width",m->Info->HorizontalResolution);hex("gop.height",m->Info->VerticalResolution);
            hex("gop.format",m->Info->PixelFormat);hex("gop.pixels-per-scanline",m->Info->PixelsPerScanLine);
        } else display_capture_ok=FALSE;
        if(m->FrameBufferSize) {
            if(m->FrameBufferSize-1>~(UINT64)0-m->FrameBufferBase){display_capture_ok=FALSE;continue;}
            display_ranges[display_range_count++]=(struct blocked_range){m->FrameBufferBase,m->FrameBufferSize};
        }
    }
    bs->FreePool(handles);
}
static BOOLEAN capture_memory_map(void) {
    UINTN capacity=0,key=0;UINT32 version=0;
    EFI_STATUS e=bs->GetMemoryMap(&capacity,NULL,&key,&memory_descriptor_size,&version);
    if(e!=EFI_BUFFER_TOO_SMALL || capacity>0x100000 || memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR) ||
       memory_descriptor_size>1024)return FALSE;
    capacity+=32*memory_descriptor_size;
    e=bs->AllocatePool(EfiBootServicesData,capacity,(void **)&memory_map);if(EFI_ERROR(e))return FALSE;
    memory_map_size=capacity;
    e=bs->GetMemoryMap(&memory_map_size,(void *)memory_map,&key,&memory_descriptor_size,&version);
    if(EFI_ERROR(e) || memory_map_size>capacity || memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR) ||
       memory_descriptor_size>1024 || memory_map_size%memory_descriptor_size)
        {bs->FreePool(memory_map);memory_map=NULL;memory_map_size=0;return FALSE;}
    hex("efi.memory-map-descriptor-version",version);
    for(UINTN i=0;i<memory_map_size;i+=memory_descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d=(const void *)(memory_map+i);
        emit("EFI_MEMORY");hex("memory.type",d->Type);hex("memory.base",d->PhysicalStart);
        hex("memory.pages",d->NumberOfPages);hex("memory.attributes",d->Attribute);
    }
    return TRUE;
}
static BOOLEAN mapped_image(UINT64 address,UINTN length) {
    if(!address || !length || length>0x800000 || length>~(UINT64)0-address || !memory_map ||
       memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR) || memory_descriptor_size>memory_map_size)return FALSE;
    UINT64 cursor=address,end=address+length;
    for(UINTN attempts=0;cursor<end && attempts<4096;++attempts) {
        UINT64 next=cursor;
        for(UINTN i=0;i+memory_descriptor_size<=memory_map_size;i+=memory_descriptor_size) {
            const EFI_MEMORY_DESCRIPTOR *d=(const void *)(memory_map+i);
            if(d->Type!=EfiBootServicesCode && d->Type!=EfiBootServicesData &&
               d->Type!=EfiRuntimeServicesCode && d->Type!=EfiRuntimeServicesData)continue;
            if(d->Attribute&EFI_MEMORY_RP || d->NumberOfPages>~(UINT64)0/4096)continue;
            UINT64 bytes=d->NumberOfPages*4096;
            if(cursor>=d->PhysicalStart && cursor-d->PhysicalStart<bytes) {
                UINT64 remain=bytes-(cursor-d->PhysicalStart);
                next=remain>=end-cursor?end:cursor+remain;break;
            }
        }
        if(next==cursor)return FALSE;cursor=next;
    }
    return cursor==end;
}
static BOOLEAN save_loaded_image(EFI_FILE_PROTOCOL *directory,EFI_HANDLE image_handle,const CHAR16 *filename) {
    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
    EFI_STATUS e=bs->HandleProtocol(image_handle,&loaded_guid,(void **)&im);
    if(EFI_ERROR(e) || !im || !mapped_image((UINTN)im->ImageBase,(UINTN)im->ImageSize))return FALSE;
    EFI_FILE_PROTOCOL *f=NULL;
    e=directory->Open(directory,&f,(CHAR16 *)filename,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(e))return FALSE;
    UINTN size=(UINTN)im->ImageSize;e=f->Write(f,&size,im->ImageBase);
    EFI_STATUS flushed=f->Flush(f),closed=f->Close(f);
    hex("image.handle",(UINTN)image_handle);hex("image.loaded-base",(UINTN)im->ImageBase);
    hex("image.bytes",im->ImageSize);hex("image.system-table",(UINTN)im->SystemTable);
    return !EFI_ERROR(e) && size==im->ImageSize && !EFI_ERROR(flushed) && !EFI_ERROR(closed);
}
static void capture_drivers(void) {
    if(!memory_map || !gpu_root || !gpu_root_handle)return;
    CHAR16 folder[]=L"RA0000";for(UINTN i=0;i<6;++i)folder[i]=logname[i+1];
    EFI_FILE_PROTOCOL *directory=NULL;
    EFI_STATUS e=root->Open(root,&directory,folder,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
    if(EFI_ERROR(e)){hex("images.directory-error",e);return;}
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN count=0;
    e=bs->OpenProtocolInformation(gpu_root_handle,&rb_guid,&entries,&count);
    if(!EFI_ERROR(e)) {
        EFI_HANDLE driver_image=NULL;UINTN drivers=0;
        for(UINTN i=0;i<count && i<256;++i)if(entries[i].Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER) {
            EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
            if(!EFI_ERROR(bs->HandleProtocol(entries[i].AgentHandle,&binding_guid,(void **)&binding)) && binding) {
                driver_image=binding->ImageHandle;++drivers;
                hex("pci-driver.start",(UINTN)binding->Start);hex("pci-driver.stop",(UINTN)binding->Stop);
            }
        }
        if(drivers==1){emit("IMAGE=PCI-BUS.MEM");hex("image.saved",save_loaded_image(directory,driver_image,L"PCI-BUS.MEM"));}
        bs->FreePool(entries);
    }
    EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *host=NULL;
    e=bs->HandleProtocol(gpu_root->ParentHandle,&host_guid,(void **)&host);
    if(!EFI_ERROR(e) && host) {
        UINT64 functions[2]={(UINTN)host->NotifyPhase,(UINTN)host->PreprocessController};
        hex("host.notify-phase",functions[0]);hex("host.preprocess",functions[1]);
        EFI_HANDLE *images=NULL;UINTN image_count=0;
        e=bs->LocateHandleBuffer(ByProtocol,&loaded_guid,NULL,&image_count,&images);
        if(!EFI_ERROR(e)) {
            for(UINTN f=0;f<2;++f) {
                EFI_HANDLE owner=NULL;UINTN matches=0;
                for(UINTN i=0;i<image_count && i<2048;++i) {
                    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
                    if(EFI_ERROR(bs->HandleProtocol(images[i],&loaded_guid,(void **)&im)) || !im)continue;
                    if(functions[f]>=(UINTN)im->ImageBase && functions[f]-(UINTN)im->ImageBase<im->ImageSize)
                        {owner=images[i];++matches;}
                }
                if(matches==1) {
                    emit(f?"IMAGE=HOST-PRE.MEM":"IMAGE=HOST-NOT.MEM");
                    hex("image.saved",save_loaded_image(directory,owner,f?L"HOST-PRE.MEM":L"HOST-NOT.MEM"));
                }
            }
            bs->FreePool(images);
        }
    }
    if(EFI_ERROR(directory->Close(directory)))log_ok=FALSE;
}
