/* Included by test_probe.c. All framebuffer and protocol accesses are mocks. */
static EFI_GRAPHICS_OUTPUT_PROTOCOL hg;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE hm;
static EFI_PCI_IO_PROTOCOL hp;
static UINT32 pixels[12*6],pixel_original[12*6];
static UINTN memory_writes,fail_memory_write,native_calls;
static UINT8 pool_bytes[256];
static UINT64 recorded_address,recorded_source;
static UINTN recorded_count;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH recorded_width;
static EFI_STATUS EFIAPI adapter_pool(EFI_MEMORY_TYPE type,UINTN size,void **out) {
    (void)type;if(size>sizeof(pool_bytes))return EFI_OUT_OF_RESOURCES;*out=pool_bytes;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI adapter_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *out) {
    (void)self;if(width!=EfiPciWidthUint32 || a<boot_fb || (a-boot_fb)&3 ||
        count>sizeof(pixels)/4 || a-boot_fb>sizeof(pixels)-count*4)return EFI_INVALID_PARAMETER;
    memcpy(out,(UINT8 *)pixels+a-boot_fb,count*4);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI adapter_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *in) {
    (void)self;if((width!=EfiPciWidthUint32 && width!=EfiPciWidthFillUint32) || a<boot_fb || (a-boot_fb)&3 ||
        count>sizeof(pixels)/4 || a-boot_fb>sizeof(pixels)-count*4)return EFI_INVALID_PARAMETER;
    ++memory_writes;
    UINT32 *dest=(void *)((UINT8 *)pixels+a-boot_fb);
    for(UINTN i=0;i<count;++i)dest[i]=((UINT32 *)in)[width==EfiPciWidthFillUint32?0:i];
    return memory_writes==fail_memory_write?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI adapter_native(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH w,
    UINT8 b,UINT64 o,UINTN n,void *v) {
    (void)self;(void)w;(void)b;(void)o;(void)n;(void)v;++native_calls;return EFI_SUCCESS;
}
static void adapter_setup(void) {
    setup_preflight();
    EFI_GRAPHICS_OUTPUT_PROTOCOL zg={0};EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE zm={0};EFI_PCI_IO_PROTOCOL zp={0};
    hg=zg;hm=zm;hp=zp;hg.Mode=&hm;boot_gop=&hg;boot_pci=&hp;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info={0};boot_info=info;
    boot_info.HorizontalResolution=8;boot_info.VerticalResolution=6;boot_info.PixelsPerScanLine=12;
    boot_info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
    hm.Info=&boot_info;hm.SizeOfInfo=sizeof(boot_info);hm.FrameBufferBase=MOVE_OLD_BAR1;hm.FrameBufferSize=sizeof(pixels);hm.MaxMode=3;
    hp.Mem.Read=adapter_native;hp.Mem.Write=adapter_native;
    boot_gop_original=hg;boot_mode_original=hm;boot_pci_original=hp;
    boot_fb=MOVE_OLD_BAR1;boot_prepared=TRUE;boot_installed=FALSE;boot_large=FALSE;
    boot_bar_index[0]=0;boot_bar_index[1]=1;boot_bar_index[2]=3;
    boot_bar_max_is_end[0]=FALSE;boot_bar_max_is_end[1]=FALSE;
    boot_blt_calls=0;boot_blt_errors=0;memory_writes=0;fail_memory_write=0;native_calls=0;
    for(UINTN i=0;i<72;++i)pixels[i]=0x23450000+(UINT32)i;
    memcpy(pixel_original,pixels,sizeof(pixels));
    fake_root.Mem.Read=adapter_read;fake_root.Mem.Write=adapter_write;
    test_bs.AllocatePool=adapter_pool;
    for(UINTN i=0;i<2;++i) {
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR d={0};d.Desc=0x8a;d.Len=sizeof(d)-3;d.ResType=0;
        d.AddrSpaceGranularity=64;d.SpecificFlag=6;d.AddrLen=i?32ULL<<20:256ULL<<20;
        d.AddrRangeMin=i?MOVE_OLD_BAR3:MOVE_OLD_BAR1;d.AddrRangeMax=d.AddrLen-1;
        memcpy(boot_bar_template[i],&d,sizeof(d));boot_bar_template[i][sizeof(d)]=0x79;boot_bar_template[i][sizeof(d)+1]=0;
    }
}
EXPORT int test_handoff_pixels_stride_and_roundtrip(void) {
    adapter_setup();EFI_GRAPHICS_OUTPUT_BLT_PIXEL color={1,2,3,0};UINT32 out[24]={0};
    CHECK(boot_blt(&hg,&color,EfiBltVideoFill,0,0,2,1,3,2,0)==EFI_SUCCESS);
    for(UINTN y=0;y<6;++y)for(UINTN x=0;x<12;++x)
        CHECK(pixels[y*12+x]==((y>=1 && y<3 && x>=2 && x<5)?0x30201:pixel_original[y*12+x]));
    CHECK(boot_blt(&hg,(void *)out,EfiBltVideoToBltBuffer,2,1,1,1,3,2,6*4)==EFI_SUCCESS);
    CHECK(out[7]==0x30201 && out[15]==0x30201 && !out[0] && !out[6] && !out[10]);
    out[7]=0x55;out[8]=0x66;out[9]=0x77;
    CHECK(boot_blt(&hg,(void *)out,EfiBltBufferToVideo,1,1,0,0,3,1,6*4)==EFI_SUCCESS);
    CHECK(pixels[0]==0x55 && pixels[1]==0x66 && pixels[2]==0x77);return 0;
}
EXPORT int test_handoff_overlap_copies(void) {
    for(UINTN down=0;down<2;++down) {
        adapter_setup();UINTN sy=down?0:1,dy=down?1:0;
        CHECK(boot_blt(&hg,NULL,EfiBltVideoToVideo,0,sy,1,dy,6,4,0)==EFI_SUCCESS);
        for(UINTN y=0;y<4;++y)for(UINTN x=0;x<6;++x)CHECK(pixels[(dy+y)*12+1+x]==pixel_original[(sy+y)*12+x]);
    }
    adapter_setup();CHECK(boot_blt(&hg,NULL,EfiBltVideoToVideo,0,0,2,0,6,1,0)==EFI_SUCCESS);
    for(UINTN x=0;x<6;++x)CHECK(pixels[2+x]==pixel_original[x]);return 0;
}
EXPORT int test_handoff_invalid_rectangles_and_buffers(void) {
    adapter_setup();UINT32 b[16]={0};
    CHECK(boot_blt(&hg,(void *)b,(EFI_GRAPHICS_OUTPUT_BLT_OPERATION)-1,0,0,0,0,1,1,0)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)b,EfiBltBufferToVideo,0,0,7,0,2,1,0)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)b,EfiBltVideoToBltBuffer,7,5,0,0,2,1,0)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)b,EfiBltBufferToVideo,1,0,0,0,4,1,0)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)b,EfiBltBufferToVideo,0,0,0,0,4,2,15)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)b,EfiBltBufferToVideo,0,~(UINTN)0,0,0,4,2,16)==EFI_INVALID_PARAMETER);
    CHECK(boot_blt(&hg,(void *)(~(UINTN)0-4),EfiBltVideoToBltBuffer,0,0,0,0,4,1,0)==EFI_INVALID_PARAMETER);
    CHECK(!memory_writes && same(pixels,pixel_original,sizeof(pixels)));return 0;
}
EXPORT int test_handoff_mode_and_pixel_selftest(void) {
    adapter_setup();CHECK(boot_selftest() && same(pixels,pixel_original,sizeof(pixels)));
    adapter_setup();fail_memory_write=1;CHECK(!boot_selftest() && same(pixels,pixel_original,sizeof(pixels)));
    adapter_setup();UINTN size=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info=NULL;
    CHECK(boot_query(&hg,0,&size,&info)==EFI_SUCCESS && size==sizeof(boot_info) && info!=&boot_info);
    info->HorizontalResolution=1;CHECK(boot_info.HorizontalResolution==8);
    CHECK(boot_query(&hg,1,&size,&info)==EFI_INVALID_PARAMETER);
    CHECK(boot_set_mode(&hg,1)==EFI_UNSUPPORTED && !memory_writes);
    CHECK(boot_set_mode(&hg,0)==EFI_SUCCESS);
    for(UINTN y=0;y<6;++y)for(UINTN x=0;x<12;++x)CHECK(pixels[y*12+x]==(x<8?0:pixel_original[y*12+x]));return 0;
}
static EFI_STATUS EFIAPI record_memory(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 a,UINTN n,void *v) {
    (void)self;(void)v;recorded_address=a;recorded_count=n;recorded_width=w;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI record_poll(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 a,UINT64 mask,UINT64 value,UINT64 delay,UINT64 *out) {
    (void)self;(void)mask;(void)delay;recorded_address=a;recorded_width=w;*out=value;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI record_copy(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 d,UINT64 s,UINTN n) {
    (void)self;recorded_address=d;recorded_source=s;recorded_width=w;recorded_count=n;return EFI_SUCCESS;
}
EXPORT int test_handoff_pci_translation_and_bounds(void) {
    adapter_setup();UINT32 b=0;UINT64 a=0;boot_large=TRUE;
    fake_root.Mem.Read=record_memory;fake_root.Mem.Write=record_memory;fake_root.PollMem=record_poll;fake_root.CopyMem=record_copy;
    CHECK(boot_pci_read(&hp,EfiPciIoWidthUint32,1,0x10000000,1,&b)==EFI_SUCCESS);
    CHECK(recorded_address==MOVE_NEW_BAR1+0x10000000 && !native_calls);
    CHECK(boot_pci_write(&hp,EfiPciIoWidthFillUint32,3,0,4,&b)==EFI_SUCCESS);
    CHECK(recorded_address==0x2800000000ULL && recorded_count==4 && recorded_width==EfiPciWidthFillUint32);
    CHECK(boot_bar_address(1,EfiPciIoWidthFifoUint32,(1ULL<<35)-4,100,&a));
    CHECK(!boot_bar_address(1,EfiPciIoWidthUint32,(1ULL<<35)-4,2,&a));
    CHECK(!boot_bar_address(1,(EFI_PCI_IO_PROTOCOL_WIDTH)-1,0,1,&a));
    CHECK(!boot_bar_address(1,EfiPciIoWidthUint64,1,1,&a));
    CHECK(!boot_bar_address(3,EfiPciIoWidthUint32,32ULL<<20,1,&a));
    CHECK(boot_pci_read(&hp,EfiPciIoWidthUint32,0,0,1,&b)==EFI_SUCCESS && native_calls==1);
    CHECK(boot_poll(&hp,EfiPciIoWidthUint32,1,16,0xff,0x12,20,&a)==EFI_SUCCESS && a==0x12 && recorded_address==MOVE_NEW_BAR1+16);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,1,0,3,4,8)==EFI_SUCCESS && recorded_address==MOVE_NEW_BAR1 && recorded_source==0x2800000004ULL);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,1,0,0xff,0x100000,8)==EFI_SUCCESS && recorded_source==0x100000);
    return 0;
}
EXPORT int test_handoff_bar_descriptors(void) {
    adapter_setup();boot_large=TRUE;UINT64 supports=1;void *r=NULL;
    CHECK(boot_get_bar(&hp,1,&supports,&r)==EFI_SUCCESS && !supports);
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=r;
    CHECK(d->AddrRangeMin==MOVE_NEW_BAR1 && d->AddrLen==(1ULL<<35) && d->AddrRangeMax==((1ULL<<35)-1));
    CHECK(d->AddrSpaceGranularity==64 && d->SpecificFlag==6 && ((UINT8 *)r)[sizeof(*d)]==0x79);
    CHECK(boot_get_bar(&hp,3,NULL,&r)==EFI_SUCCESS);d=r;
    CHECK(d->AddrRangeMin==0x2800000000ULL && d->AddrLen==(32ULL<<20));
    CHECK(boot_get_bar(&hp,1,NULL,NULL)==EFI_INVALID_PARAMETER);
    UINT64 off=0,len=4096;CHECK(boot_set_bar(&hp,0,1,&off,&len)==EFI_UNSUPPORTED);return 0;
}
EXPORT int test_handoff_commit_or_rollback(void) {
    adapter_setup();CHECK(boot_install());move_commit=boot_commit;
    CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS && move_retained && boot_large && pci_writes==15);
    CHECK(boot_fb==MOVE_NEW_BAR1 && hm.FrameBufferBase==MOVE_NEW_BAR1 && fake_registers[10]==0xf01 && fake_registers[1]==7);
    CHECK(boot_selftest());boot_uninstall();CHECK(boot_installed); /* Never unhook a live relocated GPU. */
    adapter_setup();CHECK(boot_install());move_commit=boot_commit;hg.Blt=NULL;
    CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS && !move_retained && !boot_large && fake_original());
    boot_uninstall();CHECK(!boot_installed && same(&hg,&boot_gop_original,sizeof(hg)) && hm.MaxMode==3);
    return 0;
}
static int retain_action(UINT64 a) {
    if(!reserved || a!=allocated_base)return MOVE_UNSAFE;
    move_retained=TRUE;return MOVE_PASS;
}
EXPORT int test_handoff_retains_owned_destination(void) {
    setup();reserved_action=retain_action;
    CHECK(allocation_trial(test_self)==ALLOCATION_RETAINED && reserved && !free_calls);
    setup();reserved_action=retain_action;scenario=8;
    CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && reserved && !free_calls);
    return 0;
}
EXPORT int test_handoff_changed_mode_and_readonly_storage(void) {
    adapter_setup();hm.FrameBufferBase+=4096;CHECK(!boot_install() && !memory_writes);
    adapter_setup();CHECK(boot_install());hm.MaxMode=2;CHECK(!boot_commit(MOVE_NEW_BAR1) && !boot_large);
    EFI_MEMORY_DESCRIPTOR m={0};m.Type=EfiBootServicesData;m.PhysicalStart=0x1000;m.NumberOfPages=2;
    memory_map=(void *)&m;memory_map_size=sizeof(m);memory_descriptor_size=sizeof(m);
    CHECK(boot_writable(0x1000,0x100));m.Attribute=EFI_MEMORY_RO;CHECK(!boot_writable(0x1000,0x100));
    m.Attribute=EFI_MEMORY_RUNTIME;CHECK(!boot_writable(0x1000,0x100));
    m.Attribute=0;CHECK(!boot_writable(0x2fff,2));memory_map=NULL;memory_map_size=0;
    return 0;
}

static UINT8 native_resources[6][sizeof(EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR)+2];
static EFI_STATUS native_status[6];
static UINTN native_queries;
static BOOLEAN native_null;
static EFI_STATUS EFIAPI native_get_bar(EFI_PCI_IO_PROTOCOL *self,UINT8 bar,UINT64 *supports,void **resources) {
    (void)self;++native_queries;if(supports)*supports=0;
    if(bar>=6)return EFI_UNSUPPORTED;
    if(EFI_ERROR(native_status[bar]))return native_status[bar];
    if(resources)*resources=native_null?NULL:native_resources[bar];
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI native_poll(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINT64 mask,UINT64 value,UINT64 delay,UINT64 *result) {
    (void)self;(void)width;(void)bar;(void)offset;(void)mask;(void)value;(void)delay;(void)result;
    ++native_calls;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI native_copy(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 dbar,UINT64 dest,UINT8 sbar,UINT64 src,UINTN count) {
    (void)self;(void)width;(void)dbar;(void)dest;(void)sbar;(void)src;(void)count;
    ++native_calls;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI native_set_bar(EFI_PCI_IO_PROTOCOL *self,UINT64 attr,UINT8 bar,UINT64 *offset,UINT64 *length) {
    (void)self;(void)attr;(void)bar;(void)offset;(void)length;++native_calls;return EFI_SUCCESS;
}
static void resource_setup(BOOLEAN compact,BOOLEAN end_address) {
    adapter_setup();native_queries=0;native_null=FALSE;
    for(UINTN i=0;i<6;++i) {
        native_status[i]=EFI_UNSUPPORTED;
        for(UINTN j=0;j<sizeof(native_resources[i]);++j)native_resources[i][j]=0;
    }
    UINT8 indices[]={0,1,compact?2:3,compact?3:5};
    const UINT64 bases[]={0xde000000,MOVE_OLD_BAR1,MOVE_OLD_BAR3,0xf000};
    const UINT64 lengths[]={16ULL<<20,256ULL<<20,32ULL<<20,0x80};
    for(UINTN i=0;i<4;++i) {
        UINT8 index=indices[i];native_status[index]=EFI_SUCCESS;
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=(void *)native_resources[index];
        d->Desc=0x8a;d->Len=sizeof(*d)-3;d->ResType=i==3?1:0;
        d->AddrSpaceGranularity=(i==1 || i==2)?64:32;d->SpecificFlag=(i==1 || i==2)?6:0;
        d->AddrRangeMin=bases[i];d->AddrLen=lengths[i];
        d->AddrRangeMax=(end_address?bases[i]:0)+lengths[i]-1;
        native_resources[index][sizeof(*d)]=0x79;
    }
    hp.GetBarAttributes=native_get_bar;hp.SetBarAttributes=native_set_bar;
    hp.PollMem=native_poll;hp.CopyMem=native_copy;boot_pci_original=hp;
}
EXPORT int test_handoff_discovers_compact_and_sparse_bars(void) {
    for(UINTN compact=0;compact<2;++compact)for(UINTN end=0;end<2;++end) {
        resource_setup((BOOLEAN)compact,(BOOLEAN)end);CHECK(boot_find_bars());
        CHECK(native_queries==6 && boot_bar_index[0]==0 && boot_bar_index[1]==1 && boot_bar_index[2]==(compact?2:3));
        CHECK(boot_bar_max_is_end[0]==end && boot_bar_max_is_end[1]==end);
        CHECK(!pci_writes && !memory_writes && !boot_installed && !boot_large);
    }
    return 0;
}
EXPORT int test_handoff_rejects_ambiguous_or_invalid_resources(void) {
    for(UINTN fault=0;fault<14;++fault) {
        resource_setup(TRUE,TRUE);
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=(void *)native_resources[2];
        switch(fault) {
            case 0:native_status[2]=EFI_UNSUPPORTED;break;
            case 1:native_status[4]=EFI_SUCCESS;memcpy(native_resources[4],native_resources[1],sizeof(native_resources[4]));break;
            case 2:d->AddrLen>>=1;break;
            case 3:d->AddrRangeMin+=4096;break;
            case 4:d->AddrSpaceGranularity=32;break;
            case 5:d->AddrTranslationOffset=4096;break;
            case 6:d->ResType=1;break;
            case 7:d->AddrRangeMax++;break;
            case 8:d->SpecificFlag=0;break;
            case 9:d->Desc=0x79;break;
            case 10:d->Len--;break;
            case 11:native_resources[2][sizeof(*d)]=0;break;
            case 12:native_status[4]=EFI_DEVICE_ERROR;break;
            case 13:native_null=TRUE;break;
        }
        CHECK(!boot_find_bars());CHECK(!pci_writes && !memory_writes && !boot_installed && !boot_large);
    }
    return 0;
}
EXPORT int test_handoff_compact_access_and_io_delegation(void) {
    resource_setup(TRUE,TRUE);CHECK(boot_find_bars());boot_large=TRUE;
    fake_root.Mem.Read=record_memory;fake_root.Mem.Write=record_memory;fake_root.PollMem=record_poll;fake_root.CopyMem=record_copy;
    UINT32 b=0;UINT64 a=0;void *r=NULL;UINT64 off=0,len=4;
    CHECK(boot_pci_read(&hp,EfiPciIoWidthUint32,2,16,1,&b)==EFI_SUCCESS && recorded_address==0x2800000010ULL);
    CHECK(boot_pci_write(&hp,EfiPciIoWidthUint32,2,32,1,&b)==EFI_SUCCESS && recorded_address==0x2800000020ULL);
    CHECK(boot_poll(&hp,EfiPciIoWidthUint32,2,48,0xff,0x12,20,&a)==EFI_SUCCESS && recorded_address==0x2800000030ULL);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,2,64,1,0,1)==EFI_SUCCESS && recorded_address==0x2800000040ULL && recorded_source==MOVE_NEW_BAR1);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,1,0,0,0,1)==EFI_SUCCESS && recorded_source==0xde000000);
    CHECK(!native_calls);
    CHECK(boot_pci_read(&hp,EfiPciIoWidthUint32,3,0,1,&b)==EFI_SUCCESS);
    CHECK(boot_pci_write(&hp,EfiPciIoWidthUint32,3,0,1,&b)==EFI_SUCCESS);
    CHECK(boot_poll(&hp,EfiPciIoWidthUint32,3,0,0xff,0x12,20,&a)==EFI_SUCCESS);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,3,0,3,0,1)==EFI_SUCCESS);
    CHECK(boot_set_bar(&hp,0,3,&off,&len)==EFI_SUCCESS && native_calls==5);
    CHECK(boot_get_bar(&hp,3,NULL,&r)==EFI_SUCCESS && ((EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)r)->ResType==1 && native_queries==7);
    CHECK(boot_copy(&hp,EfiPciIoWidthUint32,1,0,3,0,1)==EFI_INVALID_PARAMETER);
    CHECK(!boot_bar_address(2,EfiPciIoWidthUint32,32ULL<<20,1,&a));
    CHECK(boot_set_bar(&hp,0,2,&off,&len)==EFI_UNSUPPORTED && native_calls==5);
    return 0;
}
EXPORT int test_handoff_preserves_native_range_max_convention(void) {
    for(UINTN end=0;end<2;++end) {
        resource_setup(TRUE,(BOOLEAN)end);CHECK(boot_find_bars());boot_large=TRUE;
        for(UINTN i=0;i<2;++i) {
            void *r=NULL;CHECK(boot_get_bar(&hp,(UINT8)(i+1),NULL,&r)==EFI_SUCCESS);
            EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=r;
            UINT64 base=i?0x2800000000ULL:MOVE_NEW_BAR1,len=i?32ULL<<20:1ULL<<35;
            CHECK(d->AddrRangeMin==base && d->AddrLen==len && d->AddrRangeMax==(end?base:0)+len-1);
        }
    }
    return 0;
}
