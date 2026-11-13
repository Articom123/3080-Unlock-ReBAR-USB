/* SPDX-License-Identifier: MIT. Host-only tests; no firmware or hardware execution. */
#include "main.c"
#define CHECK(x) do {if(!(x))return __LINE__;}while(0)
#define EXPORT __declspec(dllexport)
static EFI_GCD_MEMORY_SPACE_DESCRIPTOR initial,working[3];
static EFI_BOOT_SERVICES test_bs;
static EFI_DXE_SERVICES test_ds;
static UINTN scenario,map_calls,allocate_calls,free_calls;
static BOOLEAN reserved;
static UINT64 allocated_base;
static EFI_HANDLE test_self=(void *)0x1234;
static EFI_STATUS EFIAPI mock_free_pool(void *p){(void)p;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI mock_map(UINTN *n,EFI_GCD_MEMORY_SPACE_DESCRIPTOR **m) {
    ++map_calls;
    if((scenario==4 && map_calls==2) || (scenario==8 && map_calls==3) || scenario==10)return EFI_DEVICE_ERROR;
    working[0]=initial;
    if(!reserved){*n=1;*m=working;return EFI_SUCCESS;}
    working[0].Length=allocated_base-initial.BaseAddress;
    working[1]=initial;working[1].BaseAddress=allocated_base;working[1].Length=RESERVATION_BYTES;
    working[1].ImageHandle=scenario==3?(void *)0xbeef:test_self;
    working[2]=initial;working[2].BaseAddress=allocated_base+RESERVATION_BYTES;
    working[2].Length=initial.BaseAddress+initial.Length-working[2].BaseAddress;
    *n=3;*m=working;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_allocate(EFI_GCD_ALLOCATE_TYPE strategy,EFI_GCD_MEMORY_TYPE type,
    UINTN alignment,UINT64 length,EFI_PHYSICAL_ADDRESS *base,EFI_HANDLE image,EFI_HANDLE device) {
    ++allocate_calls;
    if(strategy!=EfiGcdAllocateAddress || type!=EfiGcdMemoryTypeMemoryMappedIo ||
       alignment!=35 || length!=RESERVATION_BYTES || (*base&(ALIGNMENT_BYTES-1)) ||
       image!=test_self || device)return EFI_INVALID_PARAMETER;
    if(scenario==1)return EFI_OUT_OF_RESOURCES;
    allocated_base=*base;reserved=TRUE;
    if(scenario==2)return EFI_ACCESS_DENIED;
    if(scenario==7)*base+=ALIGNMENT_BYTES;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_release(EFI_PHYSICAL_ADDRESS base,UINT64 length) {
    ++free_calls;if(base!=allocated_base || length!=RESERVATION_BYTES || !reserved)return EFI_INVALID_PARAMETER;
    if(scenario==5)return EFI_DEVICE_ERROR;
    if(scenario!=6)reserved=FALSE;return EFI_SUCCESS;
}
static void setup(void) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR empty={0};initial=empty;
    initial.BaseAddress=APERTURE_FIRST;initial.Length=APERTURE_LAST-APERTURE_FIRST+1;
    initial.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;initial.Capabilities=EFI_MEMORY_UC;
    display_capture_ok=TRUE;display_range_count=0;scenario=0;map_calls=0;allocate_calls=0;free_calls=0;reserved=FALSE;
    test_bs.FreePool=mock_free_pool;bs=&test_bs;test_ds.GetMemorySpaceMap=mock_map;
    test_ds.AllocateMemorySpace=mock_allocate;test_ds.FreeMemorySpace=mock_release;ds=&test_ds;
    quiet=TRUE;logfile=NULL;root=NULL;log_ok=TRUE;reserved_action=NULL;move_commit=NULL;move_retained=FALSE;
}
EXPORT int test_reserve_verified_release(void) {
    setup();CHECK(allocation_trial(test_self)==ALLOCATION_PASS);
    CHECK(allocate_calls==1 && free_calls==1 && map_calls==3 && !reserved);
    CHECK(allocated_base==0x2000000000ULL);return 0;
}
EXPORT int test_refusal_requires_unchanged_map(void) {
    setup();scenario=1;CHECK(allocation_trial(test_self)==ALLOCATION_REFUSED);
    CHECK(allocate_calls==1 && !free_calls && !reserved);
    setup();scenario=2;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && !free_calls);return 0;
}
EXPORT int test_foreign_owner_is_not_freed(void) {
    setup();scenario=3;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE);
    CHECK(reserved && !free_calls);return 0;
}
EXPORT int test_query_failure_still_releases(void) {
    setup();scenario=4;CHECK(allocation_trial(test_self)==ALLOCATION_CLEANED_UNVERIFIED);
    CHECK(free_calls==1 && !reserved && map_calls==3);return 0;
}
EXPORT int test_release_failure_blocks_handoff(void) {
    setup();scenario=5;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && reserved);
    setup();scenario=6;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && reserved);
    setup();scenario=8;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && !reserved);return 0;
}
EXPORT int test_wrong_address_is_not_freed(void) {
    setup();scenario=7;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE);
    CHECK(!free_calls && reserved);return 0;
}
EXPORT int test_no_candidate_or_map_means_no_allocation(void) {
    setup();initial.ImageHandle=(void *)1;CHECK(allocation_trial(test_self)==ALLOCATION_NO_CANDIDATE);
    CHECK(!allocate_calls && !free_calls);
    setup();scenario=10;CHECK(allocation_trial(test_self)==ALLOCATION_MAP_UNAVAILABLE && !allocate_calls);
    setup();log_ok=FALSE;CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && !allocate_calls);return 0;
}
EXPORT int test_region_type_owner_alignment_and_boundary(void) {
    setup();EFI_PHYSICAL_ADDRESS base=0;EFI_GCD_MEMORY_SPACE_DESCRIPTOR old;
    CHECK(choose_region(&initial,1,&base,&old) && base==0x2000000000ULL);
    initial.GcdMemoryType=EfiGcdMemoryTypeSystemMemory;CHECK(!choose_region(&initial,1,&base,&old));
    initial.GcdMemoryType=EfiGcdMemoryTypeNonExistent;CHECK(!choose_region(&initial,1,&base,&old));
    initial.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;initial.DeviceHandle=(void *)1;CHECK(!choose_region(&initial,1,&base,&old));
    initial.DeviceHandle=NULL;initial.Attributes=EFI_MEMORY_RUNTIME;CHECK(!choose_region(&initial,1,&base,&old));
    initial.Attributes=0;initial.BaseAddress=0x2000000000ULL;initial.Length=RESERVATION_BYTES;
    CHECK(choose_region(&initial,1,&base,&old));--initial.Length;CHECK(!choose_region(&initial,1,&base,&old));
    initial.BaseAddress=APERTURE_LAST-0x1000;initial.Length=0x1001;CHECK(!choose_region(&initial,1,&base,&old));return 0;
}
EXPORT int test_framebuffer_exclusion(void) {
    setup();EFI_PHYSICAL_ADDRESS base=0;EFI_GCD_MEMORY_SPACE_DESCRIPTOR old;
    display_range_count=1;display_ranges[0]=(struct blocked_range){0x2000000000ULL,4096};
    CHECK(choose_region(&initial,1,&base,&old) && base==0x2800000000ULL);
    display_ranges[0]=(struct blocked_range){0x2800000000ULL,4096};
    CHECK(choose_region(&initial,1,&base,&old) && base==0x3000000000ULL);
    display_capture_ok=FALSE;CHECK(!choose_region(&initial,1,&base,&old));return 0;
}
EXPORT int test_map_validation(void) {
    setup();CHECK(valid_gcd_map(&initial,1));CHECK(!valid_gcd_map(NULL,1));
    initial.Length=0;CHECK(!valid_gcd_map(&initial,1));
    initial.BaseAddress=~(UINT64)0-4;initial.Length=6;CHECK(!valid_gcd_map(&initial,1));
    initial.Length=5;CHECK(valid_gcd_map(&initial,1));
    working[0]=initial;working[1]=initial;CHECK(!valid_gcd_map(working,2));return 0;
}
EXPORT int test_range_coverage_rejects_gap_and_foreign_middle(void) {
    setup();allocated_base=0x2000000000ULL;reserved=TRUE;UINTN n=0;EFI_GCD_MEMORY_SPACE_DESCRIPTOR *m=NULL;
    CHECK(mock_map(&n,&m)==EFI_SUCCESS);
    CHECK(range_matches(m,n,allocated_base,RESERVATION_BYTES,test_self,&initial));
    --m[1].Length;CHECK(!range_matches(m,n,allocated_base,RESERVATION_BYTES,test_self,&initial));
    ++m[1].Length;m[1].Capabilities^=EFI_MEMORY_WB;CHECK(!range_matches(m,n,allocated_base,RESERVATION_BYTES,test_self,&initial));
    m[1].Capabilities=initial.Capabilities;m[1].ImageHandle=(void *)42;
    CHECK(!range_matches(m,n,allocated_base,RESERVATION_BYTES,test_self,&initial));return 0;
}
EXPORT int test_gpu_change_gate(void) {
    UINT32 a[24]={0},b[24]={0};a[0]=0x220610de;a[10]=0x146d10de;a[12]=0x02410001;
    a[14]=0x1001;a[15]=0xc1c10015;a[19]=0x801;memcpy(b,a,sizeof(a));
    CHECK(gpu_allocation_unchanged(a,b));b[14]=0x1c81;CHECK(!gpu_allocation_unchanged(a,b));
    memcpy(b,a,sizeof(a));b[19]=0xf01;CHECK(!gpu_allocation_unchanged(a,b));
    memcpy(b,a,sizeof(a));b[5]=0x1234;CHECK(!gpu_allocation_unchanged(a,b));return 0;
}
EXPORT int test_image_capture_memory_bounds(void) {
    EFI_MEMORY_DESCRIPTOR d[2]={{0},{0}};d[0].PhysicalStart=0x1000;d[0].NumberOfPages=1;d[0].Type=EfiBootServicesCode;
    d[1].PhysicalStart=0x2000;d[1].NumberOfPages=1;d[1].Type=EfiBootServicesData;
    memory_map=(void *)d;memory_map_size=sizeof(d);memory_descriptor_size=sizeof(d[0]);
    CHECK(mapped_image(0x1000,0x2000));CHECK(!mapped_image(0x1000,0x2001));
    d[1].Type=EfiMemoryMappedIO;CHECK(!mapped_image(0x1000,0x2000));
    d[1].Type=EfiBootServicesData;d[1].Attribute=EFI_MEMORY_RP;CHECK(!mapped_image(0x1000,0x2000));
    CHECK(!mapped_image(~(UINT64)0-4,32));memory_descriptor_size=0;
    CHECK(!mapped_image(0x1000,36));memory_map=NULL;memory_map_size=0;return 0;
}
EXPORT int test_acpi_capture_memory_bounds(void) {
    EFI_MEMORY_DESCRIPTOR d={0};d.Type=EfiACPIReclaimMemory;d.PhysicalStart=0x1000;d.NumberOfPages=1;
    memory_map=(void *)&d;memory_map_size=sizeof(d);memory_descriptor_size=sizeof(d);
    CHECK(mapped_acpi(0x1000,0x1000));CHECK(!mapped_acpi(0x1000,0x1001));
    d.Type=EfiMemoryMappedIO;CHECK(!mapped_acpi(0x1000,36));memory_map=NULL;memory_map_size=0;return 0;
}

static UINT32 fake_registers[MOVE_REGISTERS];
static UINTN pci_writes,pci_reads,fail_write,fail_read,sample_calls,bad_order;
static int sample_scenario;
static int resize_scenario;
static BOOLEAN partial_write,persistent_write_failure;
static BOOLEAN fake_layout_value(UINT64 a,UINT32 *v) {
    const UINT32 offsets[]={0x100,0x600,0x604,0x60c,0xbb0,0xbb4,0xbb8,0xbbc,0xbc4,0xbc8};
    const UINT32 values[]={0x60010001,0xbb01000b,0x02410001,0x1801,0xc1c10015,0x100,0x460,0x1c00,0x200,0x503};
    for(UINTN i=0;i<10;++i)if(a==(MOVE_GPU|((UINT64)offsets[i]<<32))) {
        *v=values[i];
        if(offsets[i]==0x600 && resize_scenario==7)*v=0xbb01000a;
        if(offsets[i]==0x100 && resize_scenario==8)*v=0xbb010001;
        if(offsets[i]==0x600 && resize_scenario==8)*v=0x7001000b;
        if(offsets[i]==0x600 && resize_scenario==9)*v=0x1001000b;
        if(offsets[i]==0x100 && resize_scenario==10)*v=0x60110001;
        if(offsets[i]==0xbbc) {
            if(fake_registers[9]&0x800) {
                *v=0xffc00;
                if(resize_scenario==1)*v&=~(1U<<19);
                if(resize_scenario==2)*v&=~0x1000;
            } else if(resize_scenario==5 && pci_writes)*v=0xffc00;
        }
        return TRUE;
    }
    if(a==(MOVE_GPU|(0xc1cULL<<32)) && resize_scenario==8){*v=0x60010001;return TRUE;}
    return FALSE;
}
static EFI_STATUS fake_read(UINT64 a,BOOLEAN word,UINT32 *v) {
    ++pci_reads;if(pci_reads==fail_read)return EFI_DEVICE_ERROR;
    for(UINTN i=0;i<MOVE_REGISTERS;++i)if(a==move_saved[i].address && word==move_saved[i].word) {
        *v=fake_registers[i];return EFI_SUCCESS;
    }
    return !word && fake_layout_value(a,v)?EFI_SUCCESS:EFI_INVALID_PARAMETER;
}
static EFI_STATUS fake_write(UINT64 a,BOOLEAN word,UINT32 v) {
    ++pci_writes;
    for(UINTN i=0;i<MOVE_REGISTERS;++i)if(a==move_saved[i].address && word==move_saved[i].word) {
        if(i>=2 && ((fake_registers[0]|fake_registers[1])&6))++bad_order;
        BOOLEAN fail=pci_writes==fail_write || (persistent_write_failure && pci_writes>=fail_write);
        if(!fail || partial_write) {
            if(i==10 && ((v>>8)&63)==15 && !(fake_registers[9]&0x800))++bad_order;
            if(i==9 && !(v&0x800) && ((fake_registers[10]>>8)&63)!=8)++bad_order;
            if(!(i==10 && resize_scenario==3 && ((v>>8)&63)==15))fake_registers[i]=v;
            /* Model BAR address masking at the currently selected size, both
             * for normal writes and for sizing probes. */
            BOOLEAN large=((fake_registers[10]>>8)&63)==15;
            if(large) {
                fake_registers[2]=0xc;fake_registers[3]&=0xfffffff8;
                if(resize_scenario==4 && i==3 && v==0xffffffff)fake_registers[3]=0xfffffffc;
            } else {
                fake_registers[2]=(fake_registers[2]&0xf0000000)|0xc;
                if(resize_scenario==6 && i==2 && v==0xffffffff)fake_registers[2]=0xe000000c;
            }
        }
        return fail?EFI_DEVICE_ERROR:EFI_SUCCESS;
    }
    ++bad_order;return EFI_INVALID_PARAMETER;
}
static EFI_STATUS fake_sample(UINT64 a,UINT32 *out) {
    ++sample_calls;
    UINT64 bar=(fake_registers[2]&~15ULL)|((UINT64)fake_registers[3]<<32);
    UINT64 window=((UINT64)fake_registers[7]<<32)|((fake_registers[6]&0xfff0ULL)<<16);
    UINT64 last=((UINT64)fake_registers[8]<<32)|(fake_registers[6]&0xfff00000ULL)|0xfffff;
    UINT64 size=1ULL<<(20+((fake_registers[10]>>8)&63));
    if(!(fake_registers[0]&2) || !(fake_registers[1]&2) || a<bar || a+4095>=bar+size ||
       a<window || a+4095>last){++bad_order;return EFI_DEVICE_ERROR;}
    if(sample_scenario==1 && bar==MOVE_NEW_BAR1)return EFI_DEVICE_ERROR;
    if(sample_scenario==3 && sample_calls==3)return EFI_DEVICE_ERROR;
    if(sample_scenario==4 && sample_calls==1)return EFI_DEVICE_ERROR;
    for(UINTN i=0;i<MOVE_SAMPLE_WORDS;++i)out[i]=sample_scenario==5?0:(UINT32)(0x00345600+i);
    if(sample_scenario==2 && bar==MOVE_NEW_BAR1)out[7]^=0x100;
    return EFI_SUCCESS;
}
static struct move_ops fake_ops={fake_read,fake_write,fake_sample};
static void setup_move(void) {
    setup();const UINT32 original[]={3,7,0xc000000c,0xfc,0xd000000c,0xfc,0xd1f1c001,0xfc,0xfc,0x1001,0x801};
    for(UINTN i=0;i<MOVE_REGISTERS;++i){fake_registers[i]=original[i];move_saved[i].value=original[i];}
    move_prepared=TRUE;pci_writes=0;pci_reads=0;fail_write=0;fail_read=0;sample_calls=0;bad_order=0;
    sample_scenario=0;resize_scenario=0;partial_write=FALSE;persistent_write_failure=FALSE;
}
static BOOLEAN fake_original(void) {
    for(UINTN i=0;i<MOVE_REGISTERS;++i)if(fake_registers[i]!=move_saved[i].value)return FALSE;
    return TRUE;
}
EXPORT int test_move_success_and_restoration_order(void) {
    setup_move();CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS);
    CHECK(pci_writes==30 && sample_calls==3 && !bad_order && fake_original());
    CHECK(move_registers_restored && move_pixels_restored && resize_mask_verified && resize_original_mask_verified);return 0;
}
EXPORT int test_move_each_outbound_write_failure(void) {
    for(UINTN i=1;i<=15;++i)for(UINTN partial=0;partial<2;++partial) {
        setup_move();fail_write=i;partial_write=(BOOLEAN)partial;
        CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_FAILED_RESTORED);
        CHECK(fake_original() && move_registers_restored && move_pixels_restored && !bad_order);
    }
    return 0;
}
EXPORT int test_move_restore_failure_halts(void) {
    for(UINTN i=16;i<=30;++i) {
        setup_move();fail_write=i;
        CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_UNSAFE);CHECK(!bad_order);
    }
    setup_move();fail_write=4;persistent_write_failure=TRUE;
    CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_UNSAFE);CHECK(!bad_order);return 0;
}
EXPORT int test_move_readback_failures(void) {
    setup_move();CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS);UINTN total=pci_reads;
    for(UINTN i=1;i<=total;++i) {
        setup_move();fail_read=i;int result=move_transaction(&fake_ops,MOVE_NEW_BAR1);
        CHECK(!bad_order);
        if(result!=MOVE_UNSAFE)CHECK(fake_original());
        if(result==MOVE_PASS)CHECK(move_registers_restored && move_pixels_restored);
    }
    return 0;
}
EXPORT int test_move_samples_and_preconditions(void) {
    for(int mode=1;mode<=5;++mode) {
        setup_move();sample_scenario=mode;int r=move_transaction(&fake_ops,MOVE_NEW_BAR1);
        CHECK(r==(mode<=2?MOVE_FAILED_RESTORED:mode==3?MOVE_UNSAFE:MOVE_DECLINED));
        CHECK(fake_original() && !bad_order);if(mode>=4)CHECK(!pci_writes);
    }
    setup_move();CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1+4096)==MOVE_DECLINED && !pci_writes);
    setup_move();move_prepared=FALSE;CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_DECLINED && !pci_writes);
    setup_move();fake_registers[0]|=4;CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_DECLINED && !pci_writes);
    return 0;
}
static UINTN callback_count;
static int callback_result;
static int fake_reserved_action(UINT64 base) {
    ++callback_count;if(!reserved || base!=allocated_base)return MOVE_UNSAFE;
    return callback_result;
}
EXPORT int test_move_requires_verified_reservation(void) {
    setup();callback_count=0;callback_result=MOVE_PASS;reserved_action=fake_reserved_action;
    CHECK(allocation_trial(test_self)==ALLOCATION_PASS && callback_count==1 && !reserved);
    setup();callback_count=0;scenario=4;reserved_action=fake_reserved_action;
    CHECK(allocation_trial(test_self)==ALLOCATION_CLEANED_UNVERIFIED && !callback_count && !reserved);
    setup();callback_count=0;callback_result=MOVE_UNSAFE;reserved_action=fake_reserved_action;
    CHECK(allocation_trial(test_self)==ALLOCATION_UNSAFE && callback_count==1 && reserved && !free_calls);
    return 0;
}

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL fake_root;
static UINT64 unexpected_function;
static BOOLEAN audio_active;
static EFI_STATUS EFIAPI fake_root_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *buffer) {
    (void)self;if(count!=1)return EFI_INVALID_PARAMETER;
    UINT32 v=0xffffffff;
    for(UINTN i=0;i<MOVE_REGISTERS;++i)if(a==move_saved[i].address) {
        if(width!=(move_saved[i].word?EfiPciWidthUint16:EfiPciWidthUint32))return EFI_INVALID_PARAMETER;
        v=fake_registers[i];goto found;
    }
    if(a==MOVE_BRIDGE)v=0x14db1022;
    else if(a==MOVE_BRIDGE+8)v=0x06040000;
    else if(a==MOVE_BRIDGE+0x18)v=0x00010100;
    else if(a==MOVE_AUDIO)v=0x1aef10de;
    else if(a==MOVE_AUDIO+4)v=audio_active?4:0;
    else if(a==MOVE_GPU+0x10)v=0xde000000;
    else if(a==MOVE_GPU+0x24)v=0xf001;
    else if(fake_layout_value(a,&v)){}
    else if(a==unexpected_function)v=0x123410de;
found:
    if(width==EfiPciWidthUint16)*(UINT16 *)buffer=(UINT16)v;else *(UINT32 *)buffer=v;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_root_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *buffer) {
    (void)self;if(count!=1)return EFI_INVALID_PARAMETER;
    UINT32 value=width==EfiPciWidthUint16?*(UINT16 *)buffer:*(UINT32 *)buffer;
    return fake_write(a,width==EfiPciWidthUint16,value);
}
static EFI_STATUS EFIAPI fake_root_memory(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 a,UINTN count,void *buffer) {
    (void)self;if(width!=EfiPciWidthUint32 || count!=MOVE_SAMPLE_WORDS)return EFI_INVALID_PARAMETER;
    return fake_sample(a,buffer);
}
static EFI_TPL EFIAPI fake_raise(EFI_TPL tpl){(void)tpl;return TPL_APPLICATION;}
static void EFIAPI fake_restore(EFI_TPL tpl){(void)tpl;}
static void setup_preflight(void) {
    setup_move();unexpected_function=0;audio_active=FALSE;gpu_root=&fake_root;
    fake_root.Pci.Read=fake_root_read;fake_root.Pci.Write=fake_root_write;fake_root.Mem.Read=fake_root_memory;
    test_bs.RaiseTPL=fake_raise;test_bs.RestoreTPL=fake_restore;
    display_range_count=1;display_ranges[0].base=MOVE_OLD_BAR1;display_ranges[0].length=0x21c0000;
}
EXPORT int test_move_preflight_rejects_changed_hardware(void) {
    setup_preflight();CHECK(move_prepare() && !pci_writes);
    setup_preflight();audio_active=TRUE;CHECK(!move_prepare() && !pci_writes);
    setup_preflight();unexpected_function=MOVE_GPU|(1<<16);CHECK(!move_prepare() && !pci_writes);
    setup_preflight();fake_registers[0]|=4;CHECK(!move_prepare() && !pci_writes);
    setup_preflight();display_ranges[0].base+=4096;CHECK(!move_prepare() && !pci_writes);
    setup_preflight();fake_registers[8]++;CHECK(!move_prepare() && !pci_writes);
    return 0;
}
EXPORT int test_move_live_write_and_read_bounds(void) {
    setup_preflight();CHECK(move_write(MOVE_GPU+0x610,FALSE,0x1c81)==EFI_ACCESS_DENIED);
    CHECK(move_write(MOVE_GPU+4,FALSE,1)==EFI_ACCESS_DENIED && !pci_writes);
    CHECK(move_sample(MOVE_NEW_BAR1+1,move_check)==EFI_INVALID_PARAMETER);
    CHECK(move_sample(MOVE_NEW_BAR1+MOVE_SCAN_BYTES,move_check)==EFI_INVALID_PARAMETER);
    CHECK(move_sample(MOVE_OLD_BAR3,move_check)==EFI_INVALID_PARAMETER && !sample_calls);
    CHECK(move_prepare());CHECK(move_transaction(&move_live,MOVE_NEW_BAR1)==MOVE_PASS);
    CHECK(fake_original() && !bad_order);return 0;
}
EXPORT int test_resize_capability_and_size_failures(void) {
    for(int mode=1;mode<=7;++mode) {
        setup_move();resize_scenario=mode;int result=move_transaction(&fake_ops,MOVE_NEW_BAR1);
        CHECK(result==(mode==7?MOVE_DECLINED:mode>=5?MOVE_UNSAFE:MOVE_FAILED_RESTORED));
        CHECK(!bad_order);
        if(result!=MOVE_UNSAFE)CHECK(fake_original());
        if(mode==7)CHECK(!pci_writes);
        if(mode==1 || mode==2) {
            for(UINTN j=0;j<move_trace_count;++j)
                CHECK(move_trace[j].address!=RESIZE_CONTROL || move_trace[j].value!=0xf01);
        }
    }
    return 0;
}
EXPORT int test_resize_probe_and_shrink_order(void) {
    setup_move();CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS);
    CHECK(move_trace[2].address==RESIZE_OVERRIDE && move_trace[2].value==0x1c81);
    CHECK(move_trace[3].address==RESIZE_CONTROL && move_trace[3].value==0xf01);
    CHECK(move_trace[4].value==0xffffffff && move_trace[4].readback==0xfffffff8);
    CHECK(move_trace[5].value==0xffffffff && move_trace[5].readback==0xc);
    /* These assertions protect the critical shrink -> disarm -> old address
     * dependency; programming an unaligned old base at 32 GiB loses bits. */
    UINTN shrink=64,disarm=64,old_base=64;
    for(UINTN i=0;i<move_trace_count;++i) {
        if(move_trace[i].address==RESIZE_CONTROL && move_trace[i].value==0x801)shrink=i;
        if(move_trace[i].address==RESIZE_OVERRIDE && move_trace[i].value==0x1001)disarm=i;
        if(move_trace[i].address==MOVE_GPU+0x14 && move_trace[i].value==0xc000000c)old_base=i;
    }
    CHECK(shrink<disarm && disarm<old_base && old_base<64 && fake_original());return 0;
}
EXPORT int test_resize_capability_chain_bounds(void) {
    setup_move();resize_scenario=8;CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_PASS);
    CHECK(fake_original() && !bad_order);
    for(int mode=9;mode<=10;++mode) {
        setup_move();resize_scenario=mode;
        CHECK(move_transaction(&fake_ops,MOVE_NEW_BAR1)==MOVE_DECLINED && !pci_writes);
    }
    return 0;
}
#include "test_handoff.c"
