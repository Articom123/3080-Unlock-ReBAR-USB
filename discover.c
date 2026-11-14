/* SPDX-License-Identifier: MIT. Included by main.c after relocation.c. Read-only machine discovery. */
static UINT64 cfg_address(UINT64 dev,UINT32 off) {
    return dev|(off<256?(UINT64)off:((UINT64)off<<32));
}
static BOOLEAN cfg_read32(UINT64 dev,UINT32 off,UINT32 *v) {
    *v=0xffffffff;
    return gpu_root && !EFI_ERROR(gpu_root->Pci.Read(gpu_root,EfiPciWidthUint32,cfg_address(dev,off),1,v));
}
static UINT64 pci_dev(UINTN bus,UINTN dev,UINTN fn) {return ((UINT64)bus<<24)|((UINT64)dev<<16)|((UINT64)fn<<8);}

static UINT32 cpu_physical_bits(void) {
    UINT32 a=0,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000U),"c"(0));
    if(a<0x80000008U)return 36;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000008U),"c"(0));
    a&=0xff;return a<36?36:a>52?52:a;
}
static BOOLEAN refuse(const char *why) {T.refusal=why;emit("DISCOVERY REFUSED:");emit(why);return FALSE;}

/* Read-only discovery. Fills T from the live machine; any surprise disables B. */
static BOOLEAN topo_discover(void) {
    T.refusal="discovery not run";T.already_large=FALSE;
    EFI_PCI_IO_PROTOCOL *pci=NULL;UINTN seg,bus,dev,fn;
    if(!gpu_handle || !gpu_root || EFI_ERROR(bs->HandleProtocol(gpu_handle,&pci_guid,(void **)&pci)) || !pci ||
       EFI_ERROR(pci->GetLocation(pci,&seg,&bus,&dev,&fn)) || seg)return refuse("GPU or its root bridge not found");
    T.gpu=pci_dev(bus,dev,fn);T.gpu_bus=(UINT8)bus;T.audio=pci_dev(bus,dev,1);
    UINT32 v;
    if(!cfg_read32(T.gpu,0,&v) || v!=GA102_ID)return refuse("GPU is not 10de:2206");
    if(!cfg_read32(T.gpu,0x2c,&T.subsys))return refuse("GPU subsystem unreadable");
    hex("topo.gpu-subsystem",T.subsys);
    if(dev || fn)return refuse("GPU is not function 0 of device 0 on its bus");

    /* Upstream bridge: exactly one type-1 header whose secondary bus is the GPU bus. */
    EFI_HANDLE *handles=NULL;UINTN count=0,found=0;UINT64 bridge=0;
    if(EFI_ERROR(bs->LocateHandleBuffer(ByProtocol,&pci_guid,NULL,&count,&handles)))return refuse("PCI enumeration failed");
    for(UINTN i=0;i<count && i<512;++i) {
        EFI_PCI_IO_PROTOCOL *p=NULL;UINT32 hdr=0,busreg=0;UINTN s,b,d,f;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&pci_guid,(void **)&p)) || !p ||
           EFI_ERROR(p->GetLocation(p,&s,&b,&d,&f)) || s ||
           EFI_ERROR(p->Pci.Read(p,EfiPciIoWidthUint32,0xc,1,&hdr)) || ((hdr>>16)&0x7f)!=1 ||
           EFI_ERROR(p->Pci.Read(p,EfiPciIoWidthUint32,0x18,1,&busreg)))continue;
        if(((busreg>>8)&0xff)==bus){bridge=pci_dev(b,d,f);++found;}
    }
    bs->FreePool(handles);
    if(found!=1)return refuse("GPU upstream bridge not found or ambiguous");
    T.bridge=bridge;hex("topo.bridge",T.bridge);
    UINT32 root_bus=0;void *res=NULL;
    if(!EFI_ERROR(gpu_root->Configuration(gpu_root,&res)) && res) {
        const UINT8 *p=res;
        for(UINTN i=0;i<16 && *p==0x8a;++i) {
            const EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=(const void *)p;
            if(d->ResType==2){root_bus=(UINT32)d->AddrRangeMin;break;}
            p+=3+((UINTN)p[1]|((UINTN)p[2]<<8));
        }
    }
    if(((T.bridge>>24)&0xff)!=root_bus)
        return refuse("GPU sits behind a chipset or switch bridge; use a CPU-attached x16 slot");
    if(!cfg_read32(T.bridge,0,&T.bridge_id) || !cfg_read32(T.bridge,8,&T.bridge_class) ||
       (T.bridge_class>>16)!=0x0604 || !cfg_read32(T.bridge,0x18,&T.bridge_bus) ||
       ((T.bridge_bus>>8)&0xff)!=bus || ((T.bridge_bus>>16)&0xff)!=bus)
        return refuse("upstream bridge has further buses below the GPU");

    /* Extended capabilities: NVIDIA VSEC (0x610 override lives at VSEC+0x10) and ReBAR. */
    UINT32 off=0x100,visited[128];T.vsec=0;T.rebar=0;
    for(UINTN i=0;i<128 && off;++i) {
        for(UINTN j=0;j<i;++j)if(visited[j]==off)return refuse("capability list loops");
        visited[i]=off;
        if(off<0x100 || off>0xffc || (off&3) || !cfg_read32(T.gpu,off,&v) || !v || v==0xffffffff)break;
        if((v&0xffff)==0x000b && !T.vsec)T.vsec=off;
        if((v&0xffff)==0x0015 && !T.rebar)T.rebar=off;
        off=(v>>20)&0xfff;
    }
    hex("topo.vsec",T.vsec);hex("topo.rebar",T.rebar);
    if(T.vsec!=0x600)return refuse("NVIDIA VSEC not at 0x600 (unknown VBIOS layout)");
    if(!cfg_read32(T.gpu,T.vsec+4,&v) || v!=VSEC_HEADER)return refuse("NVIDIA VSEC header differs");
    if(!T.rebar)return refuse("GPU has no Resizable BAR capability");
    UINT32 c0;if(!cfg_read32(T.gpu,T.rebar+8,&c0))return refuse("ReBAR unreadable");
    UINT32 entries=(c0>>5)&7;if(!entries || entries>6)return refuse("ReBAR entry count invalid");
    UINT32 cap_of[6]={0},ctrl_of[6]={0};
    for(UINT32 e=0;e<entries;++e) {
        UINT32 cap_off=T.rebar+4+8*e,ctrl_off=cap_off+4,ctrl;
        if(!cfg_read32(T.gpu,ctrl_off,&ctrl))return refuse("ReBAR entry unreadable");
        UINT32 bar=ctrl&7;if(bar<6 && !ctrl_of[bar]){cap_of[bar]=cap_off;ctrl_of[bar]=ctrl_off;}
    }
    if(!ctrl_of[1] || !ctrl_of[0] || !ctrl_of[3])return refuse("ReBAR lacks BAR0/BAR1/BAR3 entries");
    T.bar1_cap=cap_of[1];T.bar1_ctrl=ctrl_of[1];
    UINT32 ctrl0,ctrl1,ctrl3;
    cfg_read32(T.gpu,ctrl_of[0],&ctrl0);cfg_read32(T.gpu,T.bar1_ctrl,&ctrl1);cfg_read32(T.gpu,ctrl_of[3],&ctrl3);
    cfg_read32(T.gpu,T.bar1_cap,&T.cap1);
    T.idx1=(ctrl1>>8)&63;T.len0=1ULL<<(20+((ctrl0>>8)&63));T.len1=1ULL<<(20+T.idx1);T.len3=1ULL<<(20+((ctrl3>>8)&63));
    hex("topo.bar1-size-index",T.idx1);hex("topo.bar1-cap",T.cap1);
    UINT32 snap[7]={T.rebar,cap_of[0],ctrl_of[0],cap_of[1],ctrl_of[1],cap_of[3],ctrl_of[3]};
    for(UINTN i=0;i<7;++i)T.snap_rebar[i]=snap[i];
    T.layout_n=0;
    const UINT32 lo[]={T.vsec+4,T.vsec+0xc,T.rebar,cap_of[0],ctrl_of[0],cap_of[1],cap_of[3],ctrl_of[3]};
    for(UINTN i=0;i<8;++i){T.layout_off[i]=lo[i];if(!cfg_read32(T.gpu,lo[i],&T.layout_val[i]))return refuse("layout unreadable");}
    T.layout_n=8;

    /* BARs: BAR0 32-bit memory, BAR1 and BAR3 64-bit prefetchable, BAR5 I/O. */
    UINT32 b[6];for(UINTN i=0;i<6;++i)if(!cfg_read32(T.gpu,0x10+4*(UINT32)i,&b[i]))return refuse("BAR unreadable");
    if((b[0]&7)!=0 || (b[1]&0xf)!=0xc || (b[3]&0xf)!=0xc || !(b[5]&1))return refuse("unexpected BAR types");
    T.bar0_reg=b[0];T.bar5_reg=b[5];T.old_bar0=b[0]&~0xfULL;
    T.old_bar1=(b[1]&~0xfULL)|((UINT64)b[2]<<32);T.old_bar3=(b[3]&~0xfULL)|((UINT64)b[4]<<32);
    hex("topo.bar1",T.old_bar1);hex("topo.bar3",T.old_bar3);
    if(T.old_bar1&(T.len1-1) || T.old_bar3&(T.len3-1))return refuse("BAR addresses not size-aligned");

    /* Registers the transaction may write, captured now and re-verified at preflight. */
    topo_apply();
    for(UINTN i=0;i<11;++i) {
        BOOLEAN word=i<2;UINT64 a=move_saved[i].address;UINT32 x=0;
        if(word){UINT16 w=0;if(EFI_ERROR(gpu_root->Pci.Read(gpu_root,EfiPciWidthUint16,a,1,&w)))return refuse("register unreadable");x=w;}
        else if(EFI_ERROR(gpu_root->Pci.Read(gpu_root,EfiPciWidthUint32,a,1,&x)))return refuse("register unreadable");
        T.orig[i]=x;
    }
    if(!(T.orig[0]&2) || (T.orig[0]&4) || !(T.orig[1]&2))return refuse("GPU/bridge decode state unexpected");
    if((T.orig[6]&0xf)!=1)return refuse("bridge prefetchable window is not 64-bit");
    UINT64 pbase=((UINT64)T.orig[7]<<32)|((T.orig[6]&0xfff0ULL)<<16);
    UINT64 plast=((UINT64)T.orig[8]<<32)|(T.orig[6]&0xfff00000ULL)|0xfffff;
    if(T.old_bar1<pbase || T.old_bar1+T.len1-1>plast || T.old_bar3<pbase || T.old_bar3+T.len3-1>plast)
        return refuse("BAR1/BAR3 are not inside the bridge's prefetchable window");

    BOOLEAN armed=(T.orig[9]&0xf80)==0xc80;
    if(T.idx1==REBAR_32G_INDEX && armed){T.already_large=TRUE;T.refusal="BAR1 is already 32 GiB";return FALSE;}
    if(armed || T.idx1==REBAR_32G_INDEX)return refuse("0x610 override and BAR1 size disagree");
    if((T.cap1&(1U<<(4+T.idx1)))==0)return refuse("current BAR1 size not in capability mask");

    /* Framebuffer of this GPU's GOP must sit inside BAR1. */
    T.fb_base=0;UINTN in_bar1=0;
    for(UINTN j=0;j<display_range_count;++j) {
        const struct blocked_range *r=&display_ranges[j];
        if(r->base>=T.old_bar1 && r->base-T.old_bar1<T.len1){T.fb_base=r->base;T.scan=r->length&~4095ULL;++in_bar1;}
    }
    if(in_bar1!=1)return refuse("no single firmware display framebuffer in BAR1");
    /* The move is verified by finding a non-uniform page of screen content at the old
     * address and the same bytes at the new one; search the whole visible framebuffer. */
    if(T.scan>T.len1-(T.fb_base-T.old_bar1))T.scan=T.len1-(T.fb_base-T.old_bar1);
    if(T.scan<0x100000)return refuse("firmware framebuffer smaller than 1 MiB");
    hex("topo.scan-bytes",T.scan);

    T.ap_first=0x100000000ULL;
    T.ap_last=(1ULL<<cpu_physical_bits())-1;
    T.reserve=(1ULL<<35)+T.len3;
    hex("topo.aperture-last",T.ap_last);hex("topo.reserve",T.reserve);
    T.refusal="none";return TRUE;
}
