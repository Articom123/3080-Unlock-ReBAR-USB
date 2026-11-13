/* SPDX-License-Identifier: MIT. GCD reservation and restoration around a temporary resize. */
#define MAP_LIMIT 4096
#define ALIGNMENT_BITS 35
#define ALIGNMENT_BYTES (1ULL<<ALIGNMENT_BITS)
#define RESERVATION_BYTES ((1ULL<<35)+(32ULL<<20))
#define APERTURE_FIRST 0x1860000000ULL
#define APERTURE_LAST 0xfcffffffffULL
static const char *volatile version_banner="GA102 Windows ReBAR boot test v0.6.1 -- keep 32 GiB enabled";
static int (*reserved_action)(UINT64);

enum allocation_result {ALLOCATION_PASS,ALLOCATION_NO_CANDIDATE,ALLOCATION_REFUSED,
    ALLOCATION_CLEANED_UNVERIFIED,ALLOCATION_MAP_UNAVAILABLE,ALLOCATION_UNSAFE,ALLOCATION_RETAINED};

static BOOLEAN valid_gcd_map(const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m,UINTN n) {
    if(!m || !n || n>MAP_LIMIT)return FALSE;
    UINT64 previous=0;
    for(UINTN i=0;i<n;++i) {
        if(!m[i].Length || m[i].Length-1>~(UINT64)0-m[i].BaseAddress)return FALSE;
        if(i && m[i].BaseAddress<=previous)return FALSE;
        previous=m[i].BaseAddress+m[i].Length-1;
    }
    return TRUE;
}
static BOOLEAN choose_region(const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m,UINTN n,
    EFI_PHYSICAL_ADDRESS *base,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *original) {
    if(!valid_gcd_map(m,n) || !display_capture_ok || display_range_count>32)return FALSE;
    for(UINTN i=0;i<n;++i) {
        const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *d=&m[i];
        if(d->GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo || d->ImageHandle || d->DeviceHandle ||
           (d->Attributes&EFI_MEMORY_RUNTIME))continue;
        UINT64 first=d->BaseAddress>APERTURE_FIRST?d->BaseAddress:APERTURE_FIRST;
        UINT64 last=d->BaseAddress+d->Length-1;if(last>APERTURE_LAST)last=APERTURE_LAST;
        if(first>last || first>~(UINT64)0-(ALIGNMENT_BYTES-1))continue;
        first=(first+ALIGNMENT_BYTES-1)&~(ALIGNMENT_BYTES-1);
        for(UINTN attempt=0;attempt<=display_range_count && first<=last && RESERVATION_BYTES-1<=last-first;++attempt) {
            BOOLEAN blocked=FALSE;
            for(UINTN j=0;j<display_range_count;++j) {
                const struct blocked_range *r=&display_ranges[j];
                if(!r->length || r->length-1>~(UINT64)0-r->base)return FALSE;
                UINT64 end=r->base+r->length-1;
                if(first<=end && r->base<=first+RESERVATION_BYTES-1) {
                    if(end>~(UINT64)0-ALIGNMENT_BYTES)return FALSE;
                    first=(end+ALIGNMENT_BYTES)&~(ALIGNMENT_BYTES-1);blocked=TRUE;break;
                }
            }
            if(!blocked){*base=first;*original=*d;return TRUE;}
        }
    }
    return FALSE;
}
/* Verify the whole range, including split descriptors, not just its endpoints. */
static BOOLEAN range_matches(const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m,UINTN n,
    UINT64 base,UINT64 length,EFI_HANDLE owner,const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *original) {
    if(!valid_gcd_map(m,n) || !length || length>~(UINT64)0-base)return FALSE;
    UINT64 cursor=base,end=base+length;
    for(UINTN i=0;i<n && cursor<end;++i) {
        const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *d=&m[i];
        UINT64 last=d->BaseAddress+d->Length-1;if(last<cursor)continue;
        if(d->BaseAddress>cursor || d->GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo ||
           d->ImageHandle!=owner || d->DeviceHandle || d->Attributes!=original->Attributes ||
           d->Capabilities!=original->Capabilities)return FALSE;
        cursor=last>=end-1?end:last+1;
    }
    return cursor==end;
}
static EFI_STATUS read_gcd(EFI_GCD_MEMORY_SPACE_DESCRIPTOR **m,UINTN *n) {
    *m=NULL;*n=0;EFI_STATUS e=ds->GetMemorySpaceMap(n,m);
    if(EFI_ERROR(e)){if(*m)bs->FreePool(*m);*m=NULL;return e;}
    if(!valid_gcd_map(*m,*n)){if(*m)bs->FreePool(*m);*m=NULL;return EFI_COMPROMISED_DATA;}
    return EFI_SUCCESS;
}
static void dump_gcd(const char *label,const EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m,UINTN n) {
    emit(label);hex("gcd.descriptors",n);
    for(UINTN i=0;i<n;++i) {
        emit("GCD");hex("gcd.base",m[i].BaseAddress);hex("gcd.length",m[i].Length);
        hex("gcd.type",m[i].GcdMemoryType);hex("gcd.capabilities",m[i].Capabilities);
        hex("gcd.attributes",m[i].Attributes);hex("gcd.image-owner",(UINTN)m[i].ImageHandle);
        hex("gcd.device-owner",(UINTN)m[i].DeviceHandle);
    }
}
static int allocation_trial(EFI_HANDLE self) {
    if(!self || !ds || !log_ok)return ALLOCATION_UNSAFE;
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m=NULL,original={0};UINTN n=0;
    EFI_STATUS e=read_gcd(&m,&n);hex("allocation.pre-map-status",e);
    if(EFI_ERROR(e))return ALLOCATION_MAP_UNAVAILABLE;
    EFI_PHYSICAL_ADDRESS base=0;
    BOOLEAN found=choose_region(m,n,&base,&original);bs->FreePool(m);
    if(!found){emit("RESULT=NO_FREE_MMIO_CANDIDATE");return ALLOCATION_NO_CANDIDATE;}
    UINT64 requested=base;
    hex("allocation.requested-base",requested);hex("allocation.bytes",RESERVATION_BYTES);
    emit("ALLOCATION_BEGIN. Reserve destination before any temporary PCI changes.");flush_log();
    if(!log_ok)return ALLOCATION_UNSAFE;
    e=ds->AllocateMemorySpace(EfiGcdAllocateAddress,EfiGcdMemoryTypeMemoryMappedIo,
        ALIGNMENT_BITS,RESERVATION_BYTES,&base,self,NULL);
    hex("allocation.status",e);hex("allocation.returned-base",base);
    if(EFI_ERROR(e)) {
        EFI_STATUS q=read_gcd(&m,&n);hex("allocation.refusal-map-status",q);
        if(EFI_ERROR(q))return ALLOCATION_UNSAFE;
        BOOLEAN untouched=range_matches(m,n,requested,RESERVATION_BYTES,NULL,&original);
        dump_gcd("GCD_AFTER_REFUSAL",m,n);bs->FreePool(m);
        hex("allocation.refused-range-unchanged",untouched);
        return untouched?ALLOCATION_REFUSED:ALLOCATION_UNSAFE;
    }
    if(base!=requested) {
        /* Violates AllocateAddress contract. Do not free an unverified address. */
        emit("RESULT=UNEXPECTED_ALLOCATION_ADDRESS. Stop for power-off.");return ALLOCATION_UNSAFE;
    }
    EFI_STATUS q=read_gcd(&m,&n);hex("allocation.owned-map-status",q);
    BOOLEAN observed=FALSE;
    if(!EFI_ERROR(q)) {
        observed=range_matches(m,n,base,RESERVATION_BYTES,self,&original);
        dump_gcd("GCD_WHILE_RESERVED",m,n);bs->FreePool(m);
        if(!observed){emit("RESULT=OWNERSHIP_MISMATCH. Refusing to free another owner.");return ALLOCATION_UNSAFE;}
    }
    hex("allocation.ownership-verified",observed);
    if(observed && reserved_action && log_ok) {
        int action=reserved_action(base);
        if(action==MOVE_UNSAFE)return ALLOCATION_UNSAFE;
        if(move_retained) {
            if(action!=MOVE_PASS)return ALLOCATION_UNSAFE;
            q=read_gcd(&m,&n);hex("handoff.reserved-map-status",q);
            if(EFI_ERROR(q))return ALLOCATION_UNSAFE;
            BOOLEAN owned=range_matches(m,n,base,RESERVATION_BYTES,self,&original);
            dump_gcd("GCD_RETAINED_FOR_WINDOWS",m,n);bs->FreePool(m);
            hex("handoff.reservation-owned",owned);
            return owned?ALLOCATION_RETAINED:ALLOCATION_UNSAFE;
        }
    }
    /* On a map-query failure, release our successful exact-address reservation
     * anyway. A later complete read must verify cleanup before Windows boots. */
    e=ds->FreeMemorySpace(base,RESERVATION_BYTES);hex("allocation.free-status",e);
    if(EFI_ERROR(e))return ALLOCATION_UNSAFE;
    q=read_gcd(&m,&n);hex("allocation.released-map-status",q);
    if(EFI_ERROR(q))return ALLOCATION_UNSAFE;
    BOOLEAN restored=range_matches(m,n,base,RESERVATION_BYTES,NULL,&original);
    dump_gcd("GCD_AFTER_RELEASE",m,n);bs->FreePool(m);hex("allocation.release-verified",restored);
    if(!restored)return ALLOCATION_UNSAFE;
    return observed?ALLOCATION_PASS:ALLOCATION_CLEANED_UNVERIFIED;
}
static BOOLEAN choose_trial(BOOLEAN allowed) {
    emit(version_banner);
    emit("P / Enter / Esc: diagnostics only, then Windows. Default after 20 seconds.");
    if(allowed)emit("B: resize BAR1 to 32 GiB and start Windows with the new display mapping.");
    else {
        emit("B disabled: required platform, GPU, display adapter or log check failed.");
        emit("Display adapter check stopped at:");emit(boot_preflight_stage);
    }
    emit("Experimental Windows handoff. If it fails, fully power off and boot normally.");
    emit("This boot goes directly to Windows without the FP32 unlocker.");
    if(!st->ConIn)return FALSE;st->ConIn->Reset(st->ConIn,FALSE);
    for(UINTN i=0;i<200;++i) {
        EFI_INPUT_KEY k;EFI_STATUS e=st->ConIn->ReadKeyStroke(st->ConIn,&k);
        if(e==EFI_SUCCESS) {
            if(allowed && (k.UnicodeChar=='b' || k.UnicodeChar=='B'))return TRUE;
            if(k.ScanCode==0x17 || k.UnicodeChar=='\r' || k.UnicodeChar=='p' || k.UnicodeChar=='P')return FALSE;
        } else if(e!=EFI_NOT_READY)return FALSE;
        bs->Stall(100000);
    }
    return FALSE;
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE self,EFI_SYSTEM_TABLE *table) {
    st=table;bs=table->BootServices;
    EFI_STATUS e=bs->SetWatchdogTimer(0,0,0,NULL);if(EFI_ERROR(e))stop();
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;e=bs->HandleProtocol(self,&loaded_guid,(void **)&loaded);
    if(EFI_ERROR(e) || !loaded)stop();e=open_log(loaded->DeviceHandle);if(EFI_ERROR(e) || !logfile)stop();
    emit(version_banner);EFI_TIME time;
    if(!EFI_ERROR(st->RuntimeServices->GetTime(&time,NULL))) {
        hex("year",time.Year);hex("month",time.Month);hex("day",time.Day);
        hex("hour",time.Hour);hex("minute",time.Minute);hex("second",time.Second);
    }
    BOOLEAN platform=platform_matches();hex("board-and-bios-match",platform);
    e=locate_gpu();hex("gpu.locate",e);
    BOOLEAN gpu_ok=!EFI_ERROR(e) && gpu_snapshot(before_gpu) && stock_gpu(before_gpu);
    BOOLEAN dxe_ok=locate_dxe();hex("dxe.valid",dxe_ok);hex("gpu.stock-state",gpu_ok);
    emit("Capturing allocation map, display state and firmware driver images...");quiet=TRUE;
    inventory();capture_acpi();capture_gop();
    if(capture_memory_map()) {
        capture_drivers();bs->FreePool(memory_map);memory_map=NULL;memory_map_size=0;
    } else emit("efi.memory-map-unavailable");
    if(dxe_ok) {
        EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m=NULL;UINTN n=0;e=read_gcd(&m,&n);hex("gcd.initial-status",e);
        if(!EFI_ERROR(e)){dump_gcd("GCD_INITIAL",m,n);bs->FreePool(m);}
    }
    flush_log();quiet=FALSE;
    quiet=TRUE;
    BOOLEAN move_ok=platform && gpu_ok && dxe_ok && move_prepare() && boot_prepare();
    quiet=FALSE;
    hex("move.preflight",move_ok);
    BOOLEAN run=choose_trial(move_ok && log_ok);
    if(run) {
        emit("MODE=WINDOWS_32G_HANDOFF. User pressed B.");flush_log();if(!log_ok)stop();
        e=bs->SetWatchdogTimer(120,0x52415052,0,NULL);hex("watchdog.arm",e);if(EFI_ERROR(e))stop();
        quiet=TRUE;
        BOOLEAN installed=boot_install();hex("handoff.adapters-installed",installed);
        if(!installed){emit("RESULT=ADAPTER_SETUP_FAILED. Power off and preserve log.");stop();}
        move_commit=boot_commit;
        reserved_action=relocation_trial;int result=allocation_trial(self);reserved_action=NULL;move_commit=NULL;
        e=bs->SetWatchdogTimer(0,0,0,NULL);hex("watchdog.disable",e);
        hex("allocation.result-code",result);
        UINT32 after_gpu[24]={0};BOOLEAN read_ok=gpu_snapshot(after_gpu);
        BOOLEAN verified=read_ok && (move_retained?boot_gpu_expected(after_gpu):gpu_allocation_unchanged(before_gpu,after_gpu));
        hex("GPU_HANDOFF_STATE_VERIFIED",verified);
        if(result==ALLOCATION_UNSAFE || !verified || EFI_ERROR(e) || !log_ok) {
            emit("RESULT=STOPPED. Allocation state could not be verified; fully power off.");stop();
        }
        if(result==ALLOCATION_RETAINED) {
            if(!move_retained || !boot_large)stop();
            if(!boot_selftest()){emit("RESULT=RELOCATED_DISPLAY_WRITE_FAILED. Power off.");stop();}
            hex("handoff.framebuffer-base",boot_gop->Mode->FrameBufferBase);
            emit("RESULT=32G_READY_FOR_WINDOWS. Driver acceptance remains untested.");flush_log();
            quiet=FALSE;emit("32 GiB BAR1 active; starting Windows.");quiet=TRUE;
        } else {
          if(move_retained || boot_large)stop();boot_uninstall();quiet=FALSE;
          if(result==ALLOCATION_PASS) {
            if(move_outcome==MOVE_PASS)emit("RESULT=NOT_COMMITTED_RESTORED. GPU BAR1 remains 256 MiB.");
            else if(move_outcome==MOVE_FAILED_RESTORED)emit("RESULT=RESIZE_FAILED_BUT_RESTORED. Preserve this log.");
            else emit("RESULT=RESIZE_DECLINED. No PCI writes; preserve this log.");
        }
        else if(result==ALLOCATION_NO_CANDIDATE)emit("RESULT=NO_FREE_MMIO_CANDIDATE. Diagnostics saved.");
        else if(result==ALLOCATION_REFUSED)emit("RESULT=ALLOCATION_REFUSED_UNCHANGED. Diagnostics saved.");
        else if(result==ALLOCATION_CLEANED_UNVERIFIED)emit("RESULT=RELEASE_VERIFIED_RESERVATION_UNCONFIRMED.");
          else emit("RESULT=GCD_MAP_UNAVAILABLE. No allocation attempted.");
        }
    } else emit("MODE=DIAGNOSTICS_ONLY. No GCD reservation attempted.");
    EFI_HANDLE windows=NULL;e=load_windows(self,&windows);hex("windows.load-status",e);
    if(EFI_ERROR(e) || !log_ok)stop();
    if(boot_large) {
        UINT32 final_gpu[24]={0};
        if(!gpu_snapshot(final_gpu) || !boot_gpu_expected(final_gpu)) {
            emit("RESULT=HANDOFF_STATE_CHANGED. Power off.");stop();
        }
    }
    hex("handoff.blt-calls",boot_blt_calls);hex("handoff.blt-errors",boot_blt_errors);hex("handoff.pci-calls",boot_pci_calls);
    if(boot_large)emit("HANDOFF=WINDOWS_WITH_32G_BAR1");else emit("HANDOFF=WINDOWS_WITH_ORIGINAL_BAR1");
    close_files();if(!log_ok)stop();
    e=bs->StartImage(windows,NULL,NULL);hex("windows.returned",e);stop();return EFI_ABORTED;
}
