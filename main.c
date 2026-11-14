/* SPDX-License-Identifier: MIT
 * Universal variant of 3080-ReBar-USB v0.6.1 (original by artificerlover69):
 * discovers the GPU, bridge, BAR layout, free address space and Windows install at
 * boot instead of using one machine's constants, then chains the tensor unlocker
 * (\EFI\BOOT\UNLOCKER.EFI) if present, otherwise Windows Boot Manager.
 */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
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
static BOOLEAN logging_enabled=FALSE;
static EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID dp_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID pci_guid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID rb_guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
static EFI_GUID global_var_guid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};
/* Windows ESP partition GUID: unique to each Windows install, so it is resolved at
 * boot (USB \WINGUID.TXT override, else the firmware's "Windows Boot Manager" entry). */
static EFI_GUID windows_guid;
static BOOLEAN windows_guid_known;
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
            if(windows_guid_known && h->MBRType==2 && h->SignatureType==2 && same(h->Signature,&windows_guid,16))
                {if(windows)*windows=TRUE;}
        }
        total+=n;p=(void *)((UINT8 *)p+n);
    }
    return 0;
}
static EFI_STATUS read_logging_setting(void) {
    logging_enabled=FALSE;
    EFI_FILE_PROTOCOL *file=NULL;
    EFI_STATUS e=root->Open(root,&file,L"\\REBAR-LOGGING.TXT",EFI_FILE_MODE_READ,0);
    if(e==EFI_NOT_FOUND)return EFI_SUCCESS;
    if(EFI_ERROR(e))return e;
    if(!file)return EFI_COMPROMISED_DATA;
    UINT8 value=0;UINTN bytes=1;
    e=file->Read(file,&bytes,&value);
    EFI_STATUS closed=file->Close(file);
    if(EFI_ERROR(e))return e;
    if(EFI_ERROR(closed))return closed;
    if(bytes!=1 || (value!='0' && value!='1'))return EFI_COMPROMISED_DATA;
    logging_enabled=value=='1';return EFI_SUCCESS;
}
static EFI_STATUS open_log(EFI_HANDLE own) {
    BOOLEAN usb=FALSE;
    if(!path_bytes(path(own),&usb,NULL) || !usb)return EFI_UNSUPPORTED;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_STATUS e=bs->HandleProtocol(own,&fs_guid,(void **)&fs);
    if(EFI_ERROR(e))return e;
    e=fs->OpenVolume(fs,&root);if(EFI_ERROR(e))return e;
    e=read_logging_setting();if(EFI_ERROR(e) || !logging_enabled)return e;
    /* Log at USB root; no dependence on pre-existing directories. */
    for(unsigned i=0;i<10000;++i) {
        CHAR16 *name=logname;unsigned v=i;
        for(int digit=6;digit>=3;--digit){name[digit]=(CHAR16)('0'+v%10);v/=10;}
        EFI_FILE_PROTOCOL *existing=NULL;
        e=root->Open(root,&existing,name,EFI_FILE_MODE_READ,0);
        if(e==EFI_SUCCESS){existing->Close(existing);continue;}
        if(e!=EFI_NOT_FOUND)return e;
        e=root->Open(root,&logfile,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
        return !EFI_ERROR(e) && !logfile ? EFI_COMPROMISED_DATA : e;
    }
    return EFI_VOLUME_FULL;
}
static EFI_STATUS load_windows(EFI_HANDLE self,EFI_HANDLE *image) {
    EFI_HANDLE *handles=NULL,found=NULL;UINTN count=0,matches=0;
    if(!windows_guid_known)return EFI_NOT_FOUND;
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
/* Chain: start the tensor unlocker from this same USB (\EFI\BOOT\UNLOCKER.EFI)
 * instead of Windows Boot Manager. The unlocker then boots Windows itself. */
static EFI_STATUS load_unlocker(EFI_HANDLE self,EFI_HANDLE usb,EFI_HANDLE *image) {
    EFI_DEVICE_PATH_PROTOCOL *base=path(usb);BOOLEAN on_usb=FALSE;UINTN bytes=path_bytes(base,&on_usb,NULL);
    if(!bytes || !on_usb)return EFI_UNSUPPORTED;
    const CHAR16 file[]=L"\\EFI\\BOOT\\UNLOCKER.EFI";
    UINTN node_size=4+sizeof(file),total=bytes+node_size;
    UINT8 *combined=NULL;EFI_STATUS e=bs->AllocatePool(EfiBootServicesData,total,(void **)&combined);
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
    return !EFI_ERROR(e) && loaded && loaded->DeviceHandle==usb ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}
static int hexval(CHAR16 c) {
    if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;if(c>='A' && c<='F')return c-'A'+10;return -1;
}
/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (braces optional) -> EFI_GUID. */
static BOOLEAN parse_guid(const CHAR16 *t,UINTN n,EFI_GUID *g) {
    UINT8 nib[32];UINTN k=0;
    for(UINTN i=0;i<n && k<32;++i) {
        if(t[i]=='{' || t[i]=='}' || t[i]=='-' || t[i]==' ' || t[i]=='\r' || t[i]=='\n' || t[i]==0xfeff)continue;
        int h=hexval(t[i]);if(h<0)return FALSE;nib[k++]=(UINT8)h;
    }
    if(k!=32)return FALSE;
    UINT8 b[16];for(UINTN i=0;i<16;++i)b[i]=(UINT8)(nib[2*i]<<4|nib[2*i+1]);
    g->Data1=(UINT32)b[0]<<24|(UINT32)b[1]<<16|(UINT32)b[2]<<8|b[3];
    g->Data2=(UINT16)(b[4]<<8|b[5]);g->Data3=(UINT16)(b[6]<<8|b[7]);
    for(UINTN i=0;i<8;++i)g->Data4[i]=b[8+i];
    return TRUE;
}
static BOOLEAN guid_from_override(void) {
    if(!root)return FALSE;
    EFI_FILE_PROTOCOL *f=NULL;
    if(EFI_ERROR(root->Open(root,&f,L"\\WINGUID.TXT",EFI_FILE_MODE_READ,0)))return FALSE;
    UINT8 raw[256];UINTN n=sizeof(raw);EFI_STATUS e=f->Read(f,&n,raw);f->Close(f);
    if(EFI_ERROR(e) || !n)return FALSE;
    CHAR16 text[128];UINTN m=0;
    if(n>=2 && raw[0]==0xff && raw[1]==0xfe){for(UINTN i=2;i+1<n && m<128;i+=2)text[m++]=(CHAR16)(raw[i]|raw[i+1]<<8);}
    else for(UINTN i=0;i<n && m<128;++i)text[m++]=raw[i];
    return parse_guid(text,m,&windows_guid);
}
static BOOLEAN has_text(const CHAR16 *s,UINTN max,const char *needle) {
    for(UINTN i=0;i<max && s[i];++i) {
        UINTN k=0;while(needle[k] && i+k<max && s[i+k]==(CHAR16)needle[k])++k;
        if(!needle[k])return TRUE;
    }
    return FALSE;
}
/* First BootOrder entry described as "Windows Boot Manager" with a GPT partition node. */
static BOOLEAN guid_from_boot_entries(void) {
    UINT16 order[128];UINTN size=sizeof(order);UINT32 attr=0;
    if(EFI_ERROR(st->RuntimeServices->GetVariable(L"BootOrder",&global_var_guid,&attr,&size,order)))return FALSE;
    for(UINTN i=0;i<size/2;++i) {
        CHAR16 name[]=L"Boot0000";UINT16 v=order[i];
        for(int d=7;d>=4;--d){name[d]=(CHAR16)"0123456789ABCDEF"[v&15];v>>=4;}
        static UINT8 opt[4096];UINTN n=sizeof(opt);
        if(EFI_ERROR(st->RuntimeServices->GetVariable(name,&global_var_guid,&attr,&n,opt)) || n<8)continue;
        UINT16 plen=(UINT16)(opt[4]|opt[5]<<8);
        const CHAR16 *desc=(const void *)(opt+6);UINTN dmax=(n-6)/2,dl=0;
        while(dl<dmax && desc[dl])++dl;
        if(dl>=dmax || !has_text(desc,dl,"Windows Boot Manager"))continue;
        UINT8 *p=opt+6+(dl+1)*2;UINT8 *end=p+plen;if(end>opt+n)continue;
        while(p+4<=end) {
            UINTN len=(UINTN)p[2]|((UINTN)p[3]<<8);if(len<4 || p+len>end)break;
            if(p[0]==END_DEVICE_PATH_TYPE)break;
            if(p[0]==MEDIA_DEVICE_PATH && p[1]==MEDIA_HARDDRIVE_DP && len==sizeof(HARDDRIVE_DEVICE_PATH)) {
                HARDDRIVE_DEVICE_PATH *h=(void *)p;
                if(h->MBRType==2 && h->SignatureType==2){memcpy(&windows_guid,h->Signature,16);hex("windows.boot-entry",order[i]);return TRUE;}
            }
            p+=len;
        }
    }
    return FALSE;
}
static void resolve_windows(void) {
    windows_guid_known=guid_from_override();
    if(windows_guid_known)emit("windows.guid-source=WINGUID.TXT");
    else if((windows_guid_known=guid_from_boot_entries()))emit("windows.guid-source=boot-entry");
    else emit("windows.guid-source=none (unlocker chain still works)");
    if(windows_guid_known){hex("windows.guid-data1",windows_guid.Data1);}
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
        if(seg==0){gpu_handle=handles[i];++matches;}
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

#include "topology.c"
#include "inventory.c"
#include "acpi.c"
#include "capture.c"
#include "relocation.c"
#include "discover.c"
#include "handoff.c"
#include "allocation.c"
