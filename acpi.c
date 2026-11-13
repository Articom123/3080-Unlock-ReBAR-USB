/* SPDX-License-Identifier: MIT. Bounded, read-only ACPI capture before reconnect. */
static EFI_GUID acpi20_guid=EFI_ACPI_20_TABLE_GUID,acpi10_guid=EFI_ACPI_TABLE_GUID;
static UINT8 *memory_map;
static UINTN memory_map_size,memory_descriptor_size;
static EFI_FILE_PROTOCOL *acpi_directory;
static UINTN acpi_number,acpi_bytes;
static BOOLEAN dsdt_saved;
static UINT64 read_le(const UINT8 *p,UINTN n) {
    UINT64 v=0;for(UINTN i=0;i<n;++i)v|=(UINT64)p[i]<<(i*8);return v;
}
static BOOLEAN mapped_acpi(UINT64 address,UINTN length) {
    if(!address || !length || length>0x100000 || address>~(UINT64)0-length ||
       memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR))return FALSE;
    for(UINTN i=0;i+memory_descriptor_size<=memory_map_size;i+=memory_descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d=(const void *)(memory_map+i);
        if(d->Type!=EfiACPIReclaimMemory && d->Type!=EfiACPIMemoryNVS &&
           d->Type!=EfiBootServicesData && d->Type!=EfiRuntimeServicesData)continue;
        if(d->Attribute&EFI_MEMORY_RP || d->NumberOfPages>~(UINT64)0/4096)continue;
        UINT64 bytes=d->NumberOfPages*4096;
        if(address>=d->PhysicalStart && address-d->PhysicalStart<=bytes &&
           length<=bytes-(address-d->PhysicalStart))return TRUE;
    }
    return FALSE;
}
static BOOLEAN checksum(const UINT8 *p,UINTN n) {
    UINT8 sum=0;for(UINTN i=0;i<n;++i)sum=(UINT8)(sum+p[i]);return sum==0;
}
static const UINT8 *valid_table(UINT64 address) {
    if(!mapped_acpi(address,36))return NULL;
    const UINT8 *t=(const void *)(UINTN)address;UINTN n=(UINTN)read_le(t+4,4);
    if(n<36 || !mapped_acpi(address,n) || !checksum(t,n))return NULL;
    return t;
}
static BOOLEAN save_table(const UINT8 *t) {
    UINTN n=(UINTN)read_le(t+4,4);if(acpi_number>=512 || n>0x1000000-acpi_bytes)return FALSE;
    CHAR16 name[]=L"A0000.BIN";UINTN v=acpi_number++;
    for(int i=4;i>=1;--i){name[i]=(CHAR16)('0'+v%10);v/=10;}
    EFI_FILE_PROTOCOL *f=NULL;EFI_STATUS e=acpi_directory->Open(acpi_directory,&f,name,
        EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(e)){hex("acpi.file-open-error",e);return FALSE;}
    UINTN wrote=n;e=f->Write(f,&wrote,(void *)t);EFI_STATUS flushed=f->Flush(f),closed=f->Close(f);
    hex("acpi.file-index",acpi_number-1);hex("acpi.signature",read_le(t,4));hex("acpi.length",n);
    if(EFI_ERROR(e) || wrote!=n || EFI_ERROR(flushed) || EFI_ERROR(closed))return FALSE;
    acpi_bytes+=n;return TRUE;
}
static void save_dsdt(const UINT8 *fadt) {
    if(dsdt_saved || read_le(fadt,4)!=0x50434146)return;
    UINTN n=(UINTN)read_le(fadt+4,4);UINT64 addr=n>=148 ? read_le(fadt+140,8) : 0;
    if(!addr && n>=44)addr=read_le(fadt+40,4);
    const UINT8 *dsdt=valid_table(addr);
    if(dsdt && read_le(dsdt,4)==0x54445344)dsdt_saved=save_table(dsdt);
    else emit("acpi.DSDT-unreadable");
}
static void capture_acpi(void) {
    UINTN size=0,key=0;UINT32 version=0;
    EFI_STATUS e=bs->GetMemoryMap(&size,NULL,&key,&memory_descriptor_size,&version);
    if(e!=EFI_BUFFER_TOO_SMALL || memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR) ||
       size>0x100000 || memory_descriptor_size>1024){emit("acpi.memory-map-unavailable");return;}
    size+=32*memory_descriptor_size;
    e=bs->AllocatePool(EfiBootServicesData,size,(void **)&memory_map);if(EFI_ERROR(e))return;
    memory_map_size=size;e=bs->GetMemoryMap(&memory_map_size,(void *)memory_map,&key,&memory_descriptor_size,&version);
    if(EFI_ERROR(e) || memory_map_size>size || memory_descriptor_size<sizeof(EFI_MEMORY_DESCRIPTOR) ||
       memory_descriptor_size>1024 || memory_map_size%memory_descriptor_size)
        {emit("acpi.memory-map-read-failed");goto done;}
    const UINT8 *rsdp=NULL;
    for(UINTN i=0;i<st->NumberOfTableEntries && i<256;++i) {
        EFI_CONFIGURATION_TABLE *ct=&st->ConfigurationTable[i];
        if(same(&ct->VendorGuid,&acpi20_guid,16)){rsdp=ct->VendorTable;break;}
        if(same(&ct->VendorGuid,&acpi10_guid,16))rsdp=ct->VendorTable;
    }
    if(!mapped_acpi((UINTN)rsdp,20) || !same(rsdp,"RSD PTR ",8) || !checksum(rsdp,20))
        {emit("acpi.RSDP-unreadable");goto done;}
    UINT64 address=read_le(rsdp+16,4);UINTN stride=4;
    if(rsdp[15]>=2) {
        if(!mapped_acpi((UINTN)rsdp,36)){emit("acpi.RSDP-extended-unreadable");goto done;}
        UINTN len=(UINTN)read_le(rsdp+20,4);
        if(len<36 || len>4096 || !mapped_acpi((UINTN)rsdp,len) || !checksum(rsdp,len))goto done;
        UINT64 xsdt=read_le(rsdp+24,8);if(xsdt){address=xsdt;stride=8;}
    }
    const UINT8 *sdt=valid_table(address);
    if(!sdt || read_le(sdt,4)!=(stride==8?0x54445358u:0x54445352u))
        {emit("acpi.root-table-unreadable");goto done;}
    UINTN len=(UINTN)read_le(sdt+4,4);
    if((len-36)%stride || (len-36)/stride>510)goto done;
    CHAR16 directory[]=L"RC0000";for(UINTN i=0;i<6;++i)directory[i]=logname[i+1];
    e=root->Open(root,&acpi_directory,directory,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
    if(EFI_ERROR(e)){hex("acpi.directory-error",e);goto done;}
    acpi_number=0;acpi_bytes=0;dsdt_saved=FALSE;
    if(!save_table(sdt))goto close;
    for(UINTN i=36;i<len;i+=stride) {
        const UINT8 *t=valid_table(read_le(sdt+i,stride));
        if(!t){hex("acpi.invalid-table-entry",(i-36)/stride);continue;}
        if(!save_table(t))break;save_dsdt(t);
    }
    hex("acpi.DSDT-saved",dsdt_saved);hex("acpi.saved-tables",acpi_number);
close:
    if(EFI_ERROR(acpi_directory->Close(acpi_directory)))log_ok=FALSE;
    acpi_directory=NULL;
done:
    bs->FreePool(memory_map);memory_map=NULL;memory_map_size=0;
}
