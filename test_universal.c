/* Included by test_probe.c. Host mocks for the universal (v0.7) discovery path.
 * The "Intel" machine below is built from a real nv-uefi-unlocker log of an
 * RTX 3080 20GB on an Intel board (bridge 00:01.0, BAR1 0x6000000000), a layout
 * the original v0.6.1 constants would refuse. */
#define FAKE_DEVS 4
static UINT32 fake_cfg[FAKE_DEVS][1024];
static UINT64 fake_dev_addr[FAKE_DEVS];
static EFI_PCI_IO_PROTOCOL fake_pci[FAKE_DEVS];
static EFI_HANDLE fake_handles[FAKE_DEVS]={(void *)0x101,(void *)0x102,(void *)0x103,(void *)0x104};
static UINTN fake_dev_count;
static UINT8 fake_root_resources[2*sizeof(EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR)+2];

static int fake_index(UINT64 dev) {
    for(UINTN i=0;i<fake_dev_count;++i)if(fake_dev_addr[i]==dev)return (int)i;
    return -1;
}
static EFI_STATUS EFIAPI uni_root_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *buffer) {
    (void)self;if(count!=1)return EFI_INVALID_PARAMETER;
    UINT64 dev=a&0xffffff00ULL;UINT32 off=(UINT32)(a>>32);if(!off)off=(UINT32)(a&0xff);
    int i=fake_index(dev);UINT32 v=0xffffffff;
    if(i>=0 && off<4096)v=fake_cfg[i][off/4]>>((off&3)*8);
    if(width==EfiPciWidthUint16)*(UINT16 *)buffer=(UINT16)v;else *(UINT32 *)buffer=v;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uni_root_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *buffer) {
    (void)self;(void)width;(void)a;(void)count;(void)buffer;++bad_order;return EFI_ACCESS_DENIED; /* discovery is read-only */
}
static EFI_STATUS EFIAPI uni_root_config(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,void **r) {
    (void)self;*r=fake_root_resources;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uni_pci_read(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,void *out) {
    (void)w;int i=(int)(self-fake_pci);
    for(UINTN k=0;k<n;++k)((UINT32 *)out)[k]=fake_cfg[i][off/4+k];
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uni_location(EFI_PCI_IO_PROTOCOL *self,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
    int i=(int)(self-fake_pci);UINT64 a=fake_dev_addr[i];
    *s=0;*b=(a>>24)&0xff;*d=(a>>16)&0x1f;*f=(a>>8)&7;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uni_locate(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,void *k,UINTN *n,EFI_HANDLE **h) {
    (void)t;(void)g;(void)k;*n=fake_dev_count;*h=fake_handles;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uni_handle(EFI_HANDLE h,EFI_GUID *g,void **out) {
    (void)g;for(UINTN i=0;i<fake_dev_count;++i)if(fake_handles[i]==h){*out=&fake_pci[i];return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static void put(int dev,UINT32 off,UINT32 v){fake_cfg[dev][off/4]=v;}
/* dev 0 = bridge 00:01.0, dev 1 = GPU 01:00.0, dev 2 = audio 01:00.1 */
static void intel_machine(void) {
    setup();bad_order=0;
    for(UINTN i=0;i<FAKE_DEVS;++i)for(UINTN j=0;j<1024;++j)fake_cfg[i][j]=0;
    fake_dev_count=3;fake_dev_addr[0]=pci_dev(0,1,0);fake_dev_addr[1]=pci_dev(1,0,0);fake_dev_addr[2]=pci_dev(1,0,1);
    for(UINTN i=0;i<3;++i){fake_pci[i].Pci.Read=uni_pci_read;fake_pci[i].GetLocation=uni_location;}
    put(0,0,0x460d8086);put(0,4,0x00100007);put(0,8,0x06040001);put(0,0xc,0x00010000);put(0,0x18,0x00010100);
    put(0,0x24,0x11f00001);put(0,0x28,0x60);put(0,0x2c,0x60);
    const UINT32 g[]={0x220610de,0x00100003,0x030000a1,0x00800010,0x84000000,0x0000000c,0x00000060,0x1000000c,
                      0x00000060,0x00004001,0,0x146d10de};
    for(UINT32 i=0;i<12;++i)put(1,i*4,g[i]);
    /* Extended-capability chain exactly as the unlocker log listed it. */
    const UINT32 caps[][2]={{0x100,0x25010002},{0x250,0x25810018},{0x258,0x1281001e},{0x128,0x42010004},
        {0x420,0x6001000b+0x0},{0x600,0x9001000b},{0x900,0xbb010019},{0xbb0,0xc1c10015},{0xc1c,0xd0010026},
        {0xd00,0xe0010027},{0xe00,0x00010025}};
    for(UINTN i=0;i<11;++i)put(1,caps[i][0],caps[i][1]);
    put(1,0x420,0x60010001); /* AER */
    put(1,0x604,0x02410001);put(1,0x60c,0x1801);put(1,0x610,0x1001);
    put(1,0xbb4,0x100);put(1,0xbb8,0x460);put(1,0xbbc,0x1c00);put(1,0xbc0,0x801);put(1,0xbc4,0x200);put(1,0xbc8,0x503);
    put(2,0,0x1aef10de);put(2,4,0x00100000);put(2,0x10,0x85080000);
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR d={0};d.Desc=0x8a;d.Len=sizeof(d)-3;d.ResType=2;d.AddrRangeMin=0;d.AddrRangeMax=0xff;
    memcpy(fake_root_resources,&d,sizeof(d));fake_root_resources[sizeof(d)]=0x79;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL z={0};fake_root=z;gpu_root=&fake_root;
    fake_root.Pci.Read=uni_root_read;fake_root.Pci.Write=uni_root_write;fake_root.Configuration=uni_root_config;
    test_bs.LocateHandleBuffer=uni_locate;test_bs.HandleProtocol=uni_handle;
    gpu_handle=fake_handles[1];
    display_capture_ok=TRUE;display_range_count=1;display_ranges[0]=(struct blocked_range){0x6000000000ULL,0x7e9000};
}
EXPORT int test_universal_discovers_intel_layout(void) {
    intel_machine();CHECK(topo_discover() && !bad_order);
    CHECK(T.gpu==pci_dev(1,0,0) && T.bridge==pci_dev(0,1,0) && T.subsys==0x146d10de);
    CHECK(T.vsec==0x600 && T.rebar==0xbb0 && T.bar1_cap==0xbbc && T.bar1_ctrl==0xbc0 && T.idx1==8);
    CHECK(T.old_bar0==0x84000000 && T.len0==(16ULL<<20));
    CHECK(T.old_bar1==0x6000000000ULL && T.len1==(256ULL<<20));
    CHECK(T.old_bar3==0x6010000000ULL && T.len3==(32ULL<<20));
    CHECK(T.scan==0x7e9000ULL);
    CHECK(T.fb_base==0x6000000000ULL && T.ap_first==0x100000000ULL && T.reserve==(1ULL<<35)+(32ULL<<20));
    CHECK(T.orig[0]==3 && T.orig[1]==7 && T.orig[6]==0x11f00001 && T.orig[9]==0x1001 && T.orig[10]==0x801);
    CHECK(move_saved[9].address==(pci_dev(1,0,0)|(0x610ULL<<32)) && move_saved[10].address==(pci_dev(1,0,0)|(0xbc0ULL<<32)));
    CHECK(!T.already_large);return 0;
}
EXPORT int test_universal_refuses_unsafe_layouts(void) {
    for(int fault=0;fault<9;++fault) {
        intel_machine();
        switch(fault) {
            case 0:put(0,0x18,0x00020100);break;            /* GPU bus behind another bus */
            case 1:put(1,0x600,0x9001000a);break;           /* no NVIDIA VSEC */
            case 2:put(1,0x604,0x02410002);break;           /* different VSEC */
            case 3:put(0,0x24,0x11f00000);break;            /* 32-bit prefetchable window */
            case 4:put(0,0x2c,0x5f);break;                  /* window does not cover BAR3 */
            case 5:put(1,4,0x00100007);break;               /* GPU bus mastering on */
            case 6:display_ranges[0].base=0x84000000;break; /* framebuffer not in BAR1 */
            case 7:put(1,0x610,0x1c81);break;               /* override armed while BAR1 small */
            case 8:put(1,0,0x220410de);break;               /* not a 3080 (2206) */
        }
        CHECK(!topo_discover() && !T.already_large && !bad_order);
    }
    intel_machine();put(1,0x610,0x1c81);put(1,0xbc0,0xf01);put(1,0x18,0x40);put(1,0x14,0xc);put(1,0x1c,0xc);put(1,0x20,0x48);
    put(0,0x24,0x01f10001);put(0,0x28,0x40);put(0,0x2c,0x48);
    CHECK(!topo_discover() && T.already_large);return 0;
}
EXPORT int test_universal_chipset_slot_refused(void) {
    intel_machine();fake_dev_addr[0]=pci_dev(2,0,0);   /* bridge not on the root bus */
    CHECK(!topo_discover());return 0;
}
EXPORT int test_universal_preflight_and_move_on_intel(void) {
    intel_machine();CHECK(topo_discover());
    test_bs.RaiseTPL=fake_raise;test_bs.RestoreTPL=fake_restore;fake_root.Mem.Read=fake_root_memory;
    CHECK(move_prepare() && !bad_order);
    put(2,4,0x00100002);CHECK(move_prepare());            /* HDMI audio decoding its own 32-bit BAR: fine */
    put(2,0x14,0xc);put(2,0x18,0x60);CHECK(!move_prepare());/* ...but not a prefetchable BAR in the moved window */
    put(2,0x14,0);put(2,0x18,0);
    put(2,4,0x00100000);display_ranges[0].base+=0x100000;CHECK(!move_prepare());display_ranges[0].base-=0x100000;
    put(0,0x2c,0x61);CHECK(!move_prepare());put(0,0x2c,0x60);CHECK(move_prepare());
    /* Run the real transaction against the fake register model, now at Intel values. */
    for(UINTN i=0;i<MOVE_REGISTERS;++i){fake_registers[i]=T.orig[i];move_saved[i].value=T.orig[i];}
    move_prepared=TRUE;pci_writes=0;pci_reads=0;fail_write=0;fail_read=0;sample_calls=0;bad_order=0;
    sample_scenario=0;resize_scenario=0;partial_write=FALSE;persistent_write_failure=FALSE;
    /* fake_layout_value answers at the default GA102 offsets, which match this card. */
    CHECK(move_transaction(&fake_ops,0x4000000000ULL)==MOVE_PASS && !bad_order && fake_original());
    CHECK(MOVE_NEW_BAR1==0x4000000000ULL);
    UINTN window=64;
    for(UINTN i=0;i<move_trace_count;++i)if(move_trace[i].address==MOVE_BRIDGE+0x24 && move_trace[i].value==0x01f10001)window=i;
    CHECK(window<64 && move_target[2]==0xc && move_target[3]==0x40 && move_target[4]==0xc && move_target[5]==0x48);
    CHECK(move_target[7]==0x40 && move_target[8]==0x48 && move_target[9]==0x1c81 && move_target[10]==0xf01);
    CHECK(move_transaction(&fake_ops,0x4000000000ULL+(1ULL<<34))==MOVE_DECLINED);
    /* Real Intel run (v0.7.1): top 1 MiB of the screen was blank, so the move was declined.
     * v0.7.2 searches the whole framebuffer. */
    for(UINTN i=0;i<MOVE_REGISTERS;++i)fake_registers[i]=T.orig[i];
    sample_scenario=6;bad_order=0;
    CHECK(move_transaction(&fake_ops,0x4000000000ULL)==MOVE_PASS && move_sample_offset==0x100000 && !bad_order && fake_original());
    T.scan=0x100000;for(UINTN i=0;i<MOVE_REGISTERS;++i)fake_registers[i]=T.orig[i];
    CHECK(move_transaction(&fake_ops,0x4000000000ULL)==MOVE_DECLINED && fake_original());return 0;
}
EXPORT int test_universal_region_choice_on_intel(void) {
    intel_machine();CHECK(topo_discover());
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR m[3]={{0},{0},{0}};
    m[0].BaseAddress=0x4000000000ULL;m[0].Length=0x2000000000ULL;m[0].GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;
    m[1]=m[0];m[1].BaseAddress=0x6000000000ULL;m[1].Length=0x12000000ULL;m[1].ImageHandle=(void *)7;
    m[2]=m[0];m[2].BaseAddress=0x6012000000ULL;m[2].Length=0x1fee000000ULL;
    EFI_PHYSICAL_ADDRESS base=0;EFI_GCD_MEMORY_SPACE_DESCRIPTOR old;
    CHECK(choose_region(m,3,&base,&old) && base==0x4000000000ULL);
    m[0].ImageHandle=(void *)7;CHECK(choose_region(m,3,&base,&old) && base==0x6800000000ULL);
    T.ap_last=0x67ffffffffULL;CHECK(!choose_region(m,3,&base,&old));return 0;
}
static EFI_STATUS EFIAPI uni_getvar(CHAR16 *name,EFI_GUID *g,UINT32 *attr,UINTN *size,void *data) {
    (void)g;(void)attr;
    if(name[0]=='B' && name[4]=='O'){if(*size<4)return EFI_BUFFER_TOO_SMALL;((UINT16 *)data)[0]=1;((UINT16 *)data)[1]=3;*size=4;return EFI_SUCCESS;}
    UINT8 *o=data;UINTN n=0;
    const char *desc=name[7]=='1'?"UEFI USB":"Windows Boot Manager";
    o[n++]=1;o[n++]=0;o[n++]=0;o[n++]=0;UINTN plen_at=n;n+=2;
    for(UINTN i=0;desc[i];++i){o[n++]=(UINT8)desc[i];o[n++]=0;}o[n++]=0;o[n++]=0;
    UINTN start=n;
    HARDDRIVE_DEVICE_PATH h={0};h.Header.Type=MEDIA_DEVICE_PATH;h.Header.SubType=MEDIA_HARDDRIVE_DP;
    h.Header.Length[0]=sizeof(h);h.MBRType=2;h.SignatureType=2;
    for(UINTN i=0;i<16;++i)h.Signature[i]=(UINT8)(name[7]=='1'?0x11:0xa0+i);
    memcpy(o+n,&h,sizeof(h));n+=sizeof(h);
    o[n++]=0x7f;o[n++]=0xff;o[n++]=4;o[n++]=0;
    o[plen_at]=(UINT8)(n-start);o[plen_at+1]=0;*size=n;return EFI_SUCCESS;
}
EXPORT int test_universal_empty_descriptor_is_no_bar(void) {
    /* Real Intel/AMI log: indices 0..3 = BAR0, BAR1, BAR3, I/O; index 4 = SUCCESS + end tag only. */
    resource_setup(TRUE,TRUE);native_status[4]=EFI_SUCCESS;native_resources[4][0]=0x79;native_resources[4][1]=0;
    CHECK(boot_find_bars() && boot_bar_index[0]==0 && boot_bar_index[1]==1 && boot_bar_index[2]==2);
    native_resources[4][0]=0x8a;CHECK(!boot_find_bars());return 0;
}
static UINTN real_start_calls;static EFI_HANDLE last_started;
static EFI_STATUS EFIAPI fake_real_start(EFI_HANDLE h,UINTN *n,CHAR16 **d){(void)n;(void)d;++real_start_calls;last_started=h;return EFI_SUCCESS;}
static EFI_LOADED_IMAGE_PROTOCOL li_gop,li_win,li_unl;
static UINT8 fp_win[128],fp_unl[128];
static EFI_STATUS EFIAPI hook_handle(EFI_HANDLE h,EFI_GUID *g,void **out) {
    (void)g;if(h==(void *)1)*out=&li_gop;else if(h==(void *)2)*out=&li_win;else if(h==(void *)3)*out=&li_unl;
    else return EFI_UNSUPPORTED;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_crc(void *d,UINTN n,UINT32 *c){(void)d;(void)n;*c=0x1234;return EFI_SUCCESS;}
static void file_node(UINT8 *b,const char *name) {
    UINTN n=0;b[0]=MEDIA_DEVICE_PATH;b[1]=MEDIA_FILEPATH_DP;
    while(name[n]){b[4+2*n]=(UINT8)name[n];b[5+2*n]=0;++n;}b[4+2*n]=0;b[5+2*n]=0;
    UINTN len=4+2*(n+1);b[2]=(UINT8)len;b[3]=0;b[len]=0x7f;b[len+1]=0xff;b[len+2]=4;b[len+3]=0;
}
EXPORT int test_universal_unlock_then_resize_hook(void) {
    setup();bs=&test_bs;test_bs.HandleProtocol=hook_handle;test_bs.CalculateCrc32=fake_crc;
    test_bs.RaiseTPL=fake_raise;test_bs.RestoreTPL=fake_restore;test_bs.Hdr.HeaderSize=sizeof(test_bs);
    file_node(fp_win,"\\EFI\\Microsoft\\Boot\\BOOTMGFW.EFI");file_node(fp_unl,"\\EFI\\BOOT\\UNLOCKER.EFI");
    li_win.ImageCodeType=EfiLoaderCode;li_win.FilePath=(void *)fp_win;
    li_unl.ImageCodeType=EfiLoaderCode;li_unl.FilePath=(void *)fp_unl;
    li_gop.ImageCodeType=EfiBootServicesCode;li_gop.FilePath=(void *)fp_win; /* a driver never counts */
    CHECK(is_windows_loader((void *)2) && !is_windows_loader((void *)1) && !is_windows_loader((void *)3));
    real_start_calls=0;real_start_image=fake_real_start;start_hook_fired=FALSE;usb_device=NULL;
    set_start_image(start_image_hook);start_hook_installed=TRUE;CHECK(test_bs.StartImage==start_image_hook && test_bs.Hdr.CRC32==0x1234);
    CHECK(test_bs.StartImage((void *)3,NULL,NULL)==EFI_SUCCESS && last_started==(void *)3 && !start_hook_fired);
    CHECK(test_bs.StartImage((void *)1,NULL,NULL)==EFI_SUCCESS && !start_hook_fired && test_bs.StartImage==start_image_hook);
    /* Windows Boot Manager: hook fires once, restores the real StartImage, then starts Windows. */
    CHECK(start_image_hook((void *)2,NULL,NULL)==EFI_SUCCESS && start_hook_fired && last_started==(void *)2);
    CHECK(test_bs.StartImage==fake_real_start && !start_hook_installed && real_start_calls==3);return 0;
}
EXPORT int test_universal_windows_guid_from_boot_entry(void) {
    static EFI_SYSTEM_TABLE fst;static EFI_RUNTIME_SERVICES frt;st=&fst;fst.RuntimeServices=&frt;frt.GetVariable=uni_getvar;
    windows_guid_known=FALSE;CHECK(guid_from_boot_entries());
    CHECK(((UINT8 *)&windows_guid)[0]==0xa0 && ((UINT8 *)&windows_guid)[15]==0xaf);
    EFI_GUID g;const CHAR16 t[]=L"{01234567-89ab-cdef-0123-456789abcdef}\r\n";
    CHECK(parse_guid(t,sizeof(t)/2-1,&g) && g.Data1==0x01234567 && g.Data2==0x89ab && g.Data3==0xcdef && g.Data4[0]==0x01 && g.Data4[7]==0xef);
    const CHAR16 bad[]=L"0123-xyz";CHECK(!parse_guid(bad,8,&g));return 0;
}
static EFI_FILE_PROTOCOL logging_root,logging_config,logging_file;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL logging_fs;
static UINT8 logging_usb_path[]={MESSAGING_DEVICE_PATH,MSG_USB_DP,6,0,0,0,END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,4,0};
static int logging_case;
static UINTN logging_creates;
static EFI_STATUS EFIAPI logging_read(EFI_FILE_PROTOCOL *self,UINTN *n,void *data) {
    (void)self;
    if(logging_case==4)return EFI_DEVICE_ERROR;
    if(logging_case==5){*n=0;return EFI_SUCCESS;}
    *(UINT8 *)data=logging_case==3?'x':logging_case==1?'0':'1';*n=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI logging_close(EFI_FILE_PROTOCOL *self) {
    return self==&logging_config && logging_case==6?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI logging_open(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **file,CHAR16 *name,UINT64 mode,UINT64 attr) {
    (void)self;(void)attr;
    if(mode&EFI_FILE_MODE_CREATE){++logging_creates;if(logging_case==7)return EFI_VOLUME_FULL;*file=&logging_file;return EFI_SUCCESS;}
    if(name[1]=='R' && name[2]=='E' && logging_case!=0){*file=&logging_config;return EFI_SUCCESS;}
    return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI logging_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **file) {
    (void)self;*file=&logging_root;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI logging_handle(EFI_HANDLE handle,EFI_GUID *guid,void **out) {
    (void)handle;
    if(same(guid,&dp_guid,16))*out=logging_usb_path;
    else if(same(guid,&fs_guid,16))*out=&logging_fs;
    else return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
EXPORT int test_optional_logging_and_failures(void) {
    for(logging_case=0;logging_case<8;++logging_case) {
        setup();root=NULL;logfile=NULL;logging_enabled=TRUE;logging_creates=0;
        test_bs.HandleProtocol=logging_handle;logging_fs.OpenVolume=logging_volume;
        logging_root.Open=logging_open;logging_root.Close=logging_close;
        logging_config.Read=logging_read;logging_config.Close=logging_close;
        logging_file.Close=logging_close;logging_file.Flush=logging_close;
        EFI_STATUS e=open_log((void *)1);
        if(logging_case<2) {
            CHECK(e==EFI_SUCCESS && root==&logging_root && !logging_enabled && !logfile && !logging_creates);
        } else if(logging_case==2) {
            CHECK(e==EFI_SUCCESS && logging_enabled && logfile==&logging_file && logging_creates==1);
        } else {
            CHECK(EFI_ERROR(e) && !logfile);
            CHECK(logging_creates==(logging_case==7?1:0));
        }
        close_files();CHECK(!root && !logfile);
    }
    logging_enabled=FALSE;return 0;
}
