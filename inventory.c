/* SPDX-License-Identifier: MIT. Included by main.c. PCI reads only. */
static EFI_GUID host_guid=EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL_GUID;
static EFI_GUID binding_guid=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static UINT32 before_gpu[24];
/* Indices 12..21 are filled by topo_apply(): VSEC+4/+0xc/+0x10, then the ReBAR
 * header and the BAR0, BAR1 and BAR3 cap/control pairs. */
static UINT32 gpu_offsets[22]={0,4,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24,
    0x2c,0x30,0x604,0x60c,0x610,0xbb0,0xbb4,0xbb8,0xbbc,0xbc0,0xbc4,0xbc8};

static BOOLEAN gpu_snapshot(UINT32 *values) {
    if(!gpu_root)return FALSE;
    BOOLEAN ok=TRUE;
    for(UINTN i=0;i<sizeof(gpu_offsets)/sizeof(gpu_offsets[0]);++i) {
        UINT32 off=gpu_offsets[i];values[i]=0;
        UINT64 addr=MOVE_GPU|(off<256 ? (UINT64)off : (UINT64)off<<32);
        EFI_STATUS e=gpu_root->Pci.Read(gpu_root,EfiPciWidthUint32,addr,1,&values[i]);
        hex("gpu.config.offset",off);hex("gpu.config.value",values[i]);
        if(EFI_ERROR(e)){hex("gpu.config.error",e);ok=FALSE;}
    }
    return ok && values[0]==GA102_ID && values[10]==T.subsys;
}
static BOOLEAN stock_gpu(const UINT32 *v) {
    return v[0]==GA102_ID && v[10]==T.subsys &&
        v[12]==VSEC_HEADER && (v[14]&0xf80)!=0xc80 &&
        (v[15]&0xffff)==0x15 && (v[19]&7)==1 && ((v[19]>>8)&63)==T.idx1;
}
static BOOLEAN gpu_allocation_unchanged(const UINT32 *a,const UINT32 *b) {
    if(!stock_gpu(b))return FALSE;
    /* Ignore transient PCI status and command changes made by native drivers.
     * Require every GPU BAR, override and ReBAR selector to be identical. */
    for(UINTN i=4;i<=9;++i)if(a[i]!=b[i])return FALSE;
    for(UINTN i=12;i<22;++i)if(a[i]!=b[i])return FALSE;
    return TRUE;
}
static void descriptors(const void *resources) {
    const UINT8 *p=resources;UINTN total=0;if(!p)return;
    for(unsigned i=0;i<64 && total<4096;++i) {
        if(*p==0x79)return;
        if(*p!=0x8a){hex("resource.undecoded-tag",*p);return;}
        UINTN len=(UINTN)p[1]|((UINTN)p[2]<<8);len+=3;
        if(len!=sizeof(EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) || len>4096-total)return;
        const EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=(const void *)p;
        hex("resource.type",d->ResType);hex("resource.flags",d->SpecificFlag);
        hex("resource.granularity",d->AddrSpaceGranularity);
        hex("resource.minimum",d->AddrRangeMin);hex("resource.maximum",d->AddrRangeMax);
        hex("resource.length",d->AddrLen);hex("resource.translation",d->AddrTranslationOffset);
        total+=len;p+=len;
    }
}
static void dump_path(EFI_HANDLE h) {
    const UINT8 *p=(const void *)path(h);UINTN n=path_bytes((void *)p,NULL,NULL);
    hex("path.bytes",n);
    for(UINTN i=0;i<n;i+=8){UINT64 v=0;UINTN k=n-i<8?n-i:8;memcpy(&v,p+i,k);hex("path.le64",v);}
}
static void inventory(void) {
    EFI_HANDLE *handles=NULL;UINTN count=0;
    EFI_STATUS e=bs->LocateHandleBuffer(ByProtocol,&rb_guid,NULL,&count,&handles);
    hex("roots.locate",e);
    if(!EFI_ERROR(e)) {
        for(UINTN i=0;i<count && i<64;++i) {
            EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb=NULL;
            if(EFI_ERROR(bs->HandleProtocol(handles[i],&rb_guid,(void **)&rb)))continue;
            emit("ROOT");hex("root.handle",(UINTN)handles[i]);dump_path(handles[i]);
            hex("root.parent",(UINTN)rb->ParentHandle);hex("root.segment",rb->SegmentNumber);
            void *resources=NULL;e=rb->Configuration(rb,&resources);
            hex("root.configuration",e);if(!EFI_ERROR(e))descriptors(resources);
            EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *host=NULL;
            e=bs->HandleProtocol(rb->ParentHandle,&host_guid,(void **)&host);hex("root.host-protocol",e);
            if(!EFI_ERROR(e) && host && host->GetAllocAttributes) {
                UINT64 attr=0;e=host->GetAllocAttributes(host,handles[i],&attr);
                hex("root.alloc-attributes-status",e);hex("root.alloc-attributes",attr);
            }
            EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN entries_n=0;
            e=bs->OpenProtocolInformation(handles[i],&rb_guid,&entries,&entries_n);
            if(!EFI_ERROR(e)) {
                for(UINTN j=0;j<entries_n && j<128;++j) {
                    hex("root.open.agent",(UINTN)entries[j].AgentHandle);
                    hex("root.open.controller",(UINTN)entries[j].ControllerHandle);
                    hex("root.open.attributes",entries[j].Attributes);
                    EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
                    if(!EFI_ERROR(bs->HandleProtocol(entries[j].AgentHandle,&binding_guid,(void **)&binding)) && binding) {
                        hex("root.driver.version",binding->Version);
                        EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
                        if(!EFI_ERROR(bs->HandleProtocol(binding->ImageHandle,&loaded_guid,(void **)&image)) && image) {
                            hex("root.driver.image-base",(UINTN)image->ImageBase);
                            hex("root.driver.image-size",image->ImageSize);
                        }
                    }
                }
                bs->FreePool(entries);
            }
        }
        bs->FreePool(handles);
    }
    handles=NULL;count=0;e=bs->LocateHandleBuffer(ByProtocol,&pci_guid,NULL,&count,&handles);
    hex("pci.locate",e);hex("pci.count",count);if(EFI_ERROR(e))return;
    for(UINTN i=0;i<count && i<512;++i) {
        EFI_PCI_IO_PROTOCOL *pci=NULL;UINTN seg,bus,dev,fn;UINT32 cfg[16];
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&pci_guid,(void **)&pci)) || !pci ||
           EFI_ERROR(pci->GetLocation(pci,&seg,&bus,&dev,&fn)))continue;
        emit("PCI");hex("pci.segment",seg);hex("pci.bus",bus);hex("pci.device",dev);hex("pci.function",fn);
        e=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,cfg);hex("pci.read",e);
        if(EFI_ERROR(e))continue;
        for(UINTN j=0;j<16;++j){hex("pci.offset",j*4);hex("pci.value",cfg[j]);}
        /* Header type 1 includes all upstream bridge windows at 0x18..0x30. */
        if(cfg[0]==0x220610de || ((cfg[3]>>16)&0x7f)==1)dump_path(handles[i]);
    }
    bs->FreePool(handles);
}
