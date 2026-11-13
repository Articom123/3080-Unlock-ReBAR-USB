/* SPDX-License-Identifier: MIT
 * Standalone USB EFI application, exact local machine, no unlocker dependency.
 */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/Smbios.h>
#include <IndustryStandard/Acpi10.h>
#include <Guid/Acpi.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/GraphicsOutput.h>
#include <Guid/DxeServices.h>
#include <Pi/PiDxeCis.h>

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static EFI_FILE_PROTOCOL *root, *logfile;
static BOOLEAN log_ok=TRUE;
static EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID dp_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID pci_guid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID rb_guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
static EFI_GUID smbios_guid=EFI_SMBIOS_PROTOCOL_GUID;
static EFI_GUID windows_guid={0xbcfca77e,0x5cf5,0x4208,{0xb9,0x62,0x4c,0xbb,0x03,0xa0,0x31,0x77}};
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *gpu_root;
static EFI_HANDLE gpu_root_handle;
static BOOLEAN quiet=FALSE;
static CHAR16 logname[]=L"\\RH0000.TXT";
static EFI_HANDLE gpu_handle;

void *memcpy(void *,const void *,UINTN);
static BOOLEAN same(const void *a,const void *b,UINTN n) {
    const UINT8 *x=a,*y=b; for (UINTN i=0;i<n;++i) if(x[i]!=y[i])return FALSE; return TRUE;
}
static void emit(const char *s) {
    UINTN n=0; CHAR16 text[256];
    while(s[n] && n<253) {text[n]=(CHAR16)(UINT8)s[n]; ++n;}
    text[n++]='\r';text[n++]='\n';text[n]=0;
    if(!quiet && st->ConOut) st->ConOut->OutputString(st->ConOut,text);
    if(logfile && log_ok) {
        char bytes[256]; for(UINTN i=0;i<n;++i) bytes[i]=(char)text[i];
        UINTN size=n;
        if(EFI_ERROR(logfile->Write(logfile,&size,bytes)) || size!=n)log_ok=FALSE;
    }
}
static void hex(const char *label,UINT64 value) {
    char line[192];UINTN n=0;while(label[n] && n<160){line[n]=label[n];++n;}
    line[n++]='=';line[n++]='0';line[n++]='x';
    for(int i=15;i>=0;--i)line[n++]="0123456789abcdef"[(value>>(i*4))&15];
    line[n]=0;emit(line);
}
static void flush_log(void) {
    if(logfile && EFI_ERROR(logfile->Flush(logfile)))log_ok=FALSE;
}
static void close_files(void) {
    flush_log();
    if(logfile) {if(EFI_ERROR(logfile->Close(logfile)))log_ok=FALSE; logfile=NULL;}
    if(root) {if(EFI_ERROR(root->Close(root)))log_ok=FALSE;root=NULL;}
}
static void stop(void) {
    emit("STOPPED. Fully power off, then select your usual Windows/GPU unlock boot entry.");
    close_files(); for(;;)__asm__ volatile("pause");
}
static EFI_DEVICE_PATH_PROTOCOL *path(EFI_HANDLE h) {
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;
    return EFI_ERROR(bs->HandleProtocol(h,&dp_guid,(void **)&p)) ? NULL : p;
}
/* Firmware-produced paths are bounded and must end in a single end node. */
static UINTN path_bytes(EFI_DEVICE_PATH_PROTOCOL *p,BOOLEAN *usb,BOOLEAN *windows) {
    if(usb)*usb=FALSE;if(windows)*windows=FALSE;if(!p)return 0;
    UINTN total=0;
    for(unsigned i=0;i<128;++i) {
        UINTN n=(UINTN)p->Length[0]|((UINTN)p->Length[1]<<8);
        if(n<4 || n>4096-total)return 0;
        if(p->Type==END_DEVICE_PATH_TYPE)
            return p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE && n==4 ? total+4 : 0;
        if(p->Type==MESSAGING_DEVICE_PATH &&
           (p->SubType==MSG_USB_DP || p->SubType==MSG_USB_CLASS_DP || p->SubType==MSG_USB_WWID_DP))
            {if(usb)*usb=TRUE;}
        if(p->Type==MEDIA_DEVICE_PATH && p->SubType==MEDIA_HARDDRIVE_DP && n==sizeof(HARDDRIVE_DEVICE_PATH)) {
            HARDDRIVE_DEVICE_PATH *h=(void *)p;
            if(h->MBRType==2 && h->SignatureType==2 && same(h->Signature,&windows_guid,16))
                {if(windows)*windows=TRUE;}
        }
        total+=n;p=(void *)((UINT8 *)p+n);
    }
    return 0;
}
static EFI_STATUS open_log(EFI_HANDLE own) {
    BOOLEAN usb=FALSE;
    if(!path_bytes(path(own),&usb,NULL) || !usb)return EFI_UNSUPPORTED;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_STATUS e=bs->HandleProtocol(own,&fs_guid,(void **)&fs);
    if(EFI_ERROR(e))return e;
    e=fs->OpenVolume(fs,&root);if(EFI_ERROR(e))return e;
    /* Log at USB root; no dependence on pre-existing directories. */
    for(unsigned i=0;i<10000;++i) {
        CHAR16 *name=logname;unsigned v=i;
        for(int digit=6;digit>=3;--digit){name[digit]=(CHAR16)('0'+v%10);v/=10;}
        EFI_FILE_PROTOCOL *existing=NULL;
        e=root->Open(root,&existing,name,EFI_FILE_MODE_READ,0);
        if(e==EFI_SUCCESS){existing->Close(existing);continue;}
        if(e!=EFI_NOT_FOUND)return e;
        return root->Open(root,&logfile,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    }
    return EFI_VOLUME_FULL;
}
static EFI_STATUS load_windows(EFI_HANDLE self,EFI_HANDLE *image) {
    EFI_HANDLE *handles=NULL,found=NULL;UINTN count=0,matches=0;
    EFI_STATUS e=bs->LocateHandleBuffer(ByProtocol,&fs_guid,NULL,&count,&handles);
    if(EFI_ERROR(e))return e;
    for(UINTN i=0;i<count;++i){BOOLEAN win=FALSE;
        if(path_bytes(path(handles[i]),NULL,&win) && win){found=handles[i];++matches;}}
    bs->FreePool(handles);if(matches!=1)return EFI_NOT_FOUND;
    EFI_DEVICE_PATH_PROTOCOL *base=path(found);UINTN bytes=path_bytes(base,NULL,NULL);
    const CHAR16 file[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    UINTN node_size=4+sizeof(file),total=bytes+node_size;
    UINT8 *combined=NULL;e=bs->AllocatePool(EfiBootServicesData,total,(void **)&combined);
    if(EFI_ERROR(e))return e;
    memcpy(combined,base,bytes-4);
    EFI_DEVICE_PATH_PROTOCOL *node=(void *)(combined+bytes-4);
    node->Type=MEDIA_DEVICE_PATH;node->SubType=MEDIA_FILEPATH_DP;
    node->Length[0]=(UINT8)node_size;node->Length[1]=(UINT8)(node_size>>8);
    memcpy((UINT8 *)node+4,file,sizeof(file));
    memcpy((UINT8 *)node+node_size,(UINT8 *)base+bytes-4,4);
    e=bs->LoadImage(FALSE,self,(void *)combined,NULL,0,image);bs->FreePool(combined);
    if(EFI_ERROR(e))return e;
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
    e=bs->HandleProtocol(*image,&loaded_guid,(void **)&loaded);
    return !EFI_ERROR(e) && loaded && loaded->DeviceHandle==found ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}
static BOOLEAN string_is(EFI_SMBIOS_TABLE_HEADER *record,UINT8 index,const char *expected) {
    if(!index || record->Length<4)return FALSE;
    const char *s=(const char *)record+record->Length;UINTN walked=0;
    for(UINT8 i=1;i<index;++i) {
        while(*s && walked<1024){++s;++walked;}
        if(walked>=1024 || !s[1])return FALSE;++s;++walked;
    }
    UINTN n=0;while(expected[n] && n<128){if(s[n]!=expected[n])return FALSE;++n;}
    return n<128 && s[n]==0;
}
static BOOLEAN platform_matches(void) {
    EFI_SMBIOS_PROTOCOL *smb=NULL;
    if(EFI_ERROR(bs->LocateProtocol(&smbios_guid,NULL,(void **)&smb)))return FALSE;
    EFI_SMBIOS_HANDLE h=SMBIOS_HANDLE_PI_RESERVED;
    EFI_SMBIOS_TABLE_HEADER *r=NULL;BOOLEAN board=FALSE,bios=FALSE;
    for(unsigned i=0;i<512;++i) {
        EFI_STATUS e=smb->GetNext(smb,&h,NULL,&r,NULL);
        if(e==EFI_NOT_FOUND)break;if(EFI_ERROR(e) || !r)return FALSE;
        if(r->Type==0 && r->Length>=6)bios=string_is(r,((UINT8 *)r)[5],"1.A41");
        if(r->Type==2 && r->Length>=6 && string_is(r,((UINT8 *)r)[5],"B850 GAMING PLUS WIFI6E (MS-7E80)"))board=TRUE;
    }
    return board && bios;
}
static EFI_STATUS locate_gpu(void) {
    gpu_handle=NULL;gpu_root=NULL;gpu_root_handle=NULL;
    EFI_HANDLE *handles=NULL;UINTN count=0,matches=0;
    EFI_STATUS e=bs->LocateHandleBuffer(ByProtocol,&pci_guid,NULL,&count,&handles);
    if(EFI_ERROR(e))return e;
    for(UINTN i=0;i<count;++i) {
        EFI_PCI_IO_PROTOCOL *pci=NULL;UINT32 id=0;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&pci_guid,(void **)&pci)) ||
           EFI_ERROR(pci->Pci.Read(pci,EfiPciIoWidthUint32,0,1,&id)))continue;
        if(id!=0x220610de)continue;
        UINTN seg,bus,dev,fn;
        if(EFI_ERROR(pci->GetLocation(pci,&seg,&bus,&dev,&fn)))continue;
        hex("gpu.segment",seg);hex("gpu.bus",bus);hex("gpu.device",dev);hex("gpu.function",fn);
        if(seg==0 && bus==1 && dev==0 && fn==0){gpu_handle=handles[i];++matches;}
    }
    bs->FreePool(handles);if(matches!=1)return EFI_NOT_FOUND;
    e=bs->LocateHandleBuffer(ByProtocol,&rb_guid,NULL,&count,&handles);
    if(EFI_ERROR(e))return e;
    EFI_DEVICE_PATH_PROTOCOL *gp=path(gpu_handle);UINTN gn=path_bytes(gp,NULL,NULL),best=0,ties=0;
    for(UINTN i=0;i<count;++i) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb=NULL;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&rb_guid,(void **)&rb)) || rb->SegmentNumber)continue;
        EFI_DEVICE_PATH_PROTOCOL *rp=path(handles[i]);UINTN n=path_bytes(rp,NULL,NULL);
        if(n>=8 && gn>=n && same(gp,rp,n-4)) {
            if(n>best){best=n;ties=1;gpu_root=rb;gpu_root_handle=handles[i];}else if(n==best)++ties;
        }
    }
    bs->FreePool(handles);return gpu_root && ties==1 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

#include "inventory.c"
#include "acpi.c"
#include "capture.c"
#include "relocation.c"
#include "handoff.c"
#include "allocation.c"
