/* SPDX-License-Identifier: MIT. Included before allocation.c.
 * Temporary or retained 32 GiB resize. Addresses and expected values come from T
 * (topology.c / discover.c) instead of one machine's constants.
 * All I/O goes through the root bridge, avoiding cached PCI I/O BAR addresses.
 */
#define MOVE_REGISTERS 11
#define MOVE_SAMPLE_WORDS 1024
#define MOVE_SCAN_BYTES T.scan /* whole visible framebuffer (v0.6.1: first 1 MiB only) */
enum move_result {MOVE_PASS,MOVE_DECLINED,MOVE_FAILED_RESTORED,MOVE_UNSAFE};
struct move_register {UINT64 address;UINT32 value;BOOLEAN word;};
/* Only these eleven registers may be written. Command writes are 16-bit:
 * never write the adjacent PCI status or bridge secondary-status bits. */
static struct move_register move_saved[MOVE_REGISTERS];
static void topo_apply(void) {
    const UINT64 a[MOVE_REGISTERS]={MOVE_GPU+4,MOVE_BRIDGE+4,MOVE_GPU+0x14,MOVE_GPU+0x18,MOVE_GPU+0x1c,
        MOVE_GPU+0x20,MOVE_BRIDGE+0x24,MOVE_BRIDGE+0x28,MOVE_BRIDGE+0x2c,RESIZE_OVERRIDE,RESIZE_CONTROL};
    for(UINTN i=0;i<MOVE_REGISTERS;++i){move_saved[i].address=a[i];move_saved[i].word=i<2;}
    gpu_offsets[12]=T.vsec+4;gpu_offsets[13]=T.vsec+0xc;gpu_offsets[14]=T.vsec+0x10;
    for(UINTN i=0;i<7;++i)gpu_offsets[15+i]=T.snap_rebar[i];
}
struct move_ops {
    EFI_STATUS (*read)(UINT64,BOOLEAN,UINT32 *);
    EFI_STATUS (*write)(UINT64,BOOLEAN,UINT32);
    EFI_STATUS (*sample)(UINT64,UINT32 *);
};
struct move_trace {UINT64 address;UINT32 value;EFI_STATUS status,read_status;UINT32 readback,expected;};
static struct move_trace move_trace[64];
static UINTN move_trace_count;
static UINT32 move_original[MOVE_SAMPLE_WORDS],move_check[MOVE_SAMPLE_WORDS];
static UINT64 move_sample_offset;
static BOOLEAN move_prepared,move_started,move_registers_restored,move_pixels_restored;
static int move_outcome=MOVE_DECLINED;
static EFI_STATUS move_initial_read,move_destination_read,move_restored_read;
static UINT64 move_original_hash,move_destination_hash,move_restored_hash;
static UINT32 resize_supported,resize_restored_supported;
static UINT32 move_target[MOVE_REGISTERS];
static BOOLEAN resize_mask_verified,resize_original_mask_verified;
static BOOLEAN (*move_commit)(UINT64);
static BOOLEAN move_retained;

static EFI_STATUS move_read(UINT64 a,BOOLEAN word,UINT32 *v) {
    *v=0;
    return gpu_root->Pci.Read(gpu_root,word?EfiPciWidthUint16:EfiPciWidthUint32,a,1,v);
}
static EFI_STATUS move_write(UINT64 a,BOOLEAN word,UINT32 v) {
    BOOLEAN allowed=FALSE;
    for(UINTN i=0;i<MOVE_REGISTERS;++i)if(move_saved[i].address==a && move_saved[i].word==word)allowed=TRUE;
    if(!allowed)return EFI_ACCESS_DENIED;
    return gpu_root->Pci.Write(gpu_root,word?EfiPciWidthUint16:EfiPciWidthUint32,a,1,&v);
}
static EFI_STATUS move_sample(UINT64 a,UINT32 *v) {
    /* Read only a 4 KiB page inside the observed framebuffer at old/new BAR1. */
    UINT64 fb=T.fb_base-T.old_bar1,o=MOVE_OLD_BAR1+fb,n=MOVE_NEW_BAR1+fb;
    BOOLEAN old=a>=o && a-o<=MOVE_SCAN_BYTES-sizeof(move_original);
    BOOLEAN fresh=a>=n && a-n<=MOVE_SCAN_BYTES-sizeof(move_original);
    if((a&4095) || (!old && !fresh))return EFI_INVALID_PARAMETER;
    return gpu_root->Mem.Read(gpu_root,EfiPciWidthUint32,a,MOVE_SAMPLE_WORDS,v);
}
static struct move_ops move_live={move_read,move_write,move_sample};
static BOOLEAN resize_structure(struct move_ops *io) {
    /* Layout captured at discovery must be unchanged, plus an extended-capability-chain
     * walk. The override writes are allowed only for this captured GA102 layout. */
    UINT32 v=0;
    if(T.layout_n!=8)return FALSE;
    for(UINTN i=0;i<8;++i)if(EFI_ERROR(io->read(MOVE_GPU|((UINT64)T.layout_off[i]<<32),FALSE,&v)) || v!=T.layout_val[i])return FALSE;
    UINT32 off=0x100,visited[128];BOOLEAN vendor=FALSE,rebar=FALSE;
    for(UINTN i=0;i<128;++i) {
        for(UINTN j=0;j<i;++j)if(visited[j]==off)return FALSE;
        visited[i]=off;
        if(off<0x100 || off>0xffc || (off&3) || EFI_ERROR(io->read(MOVE_GPU|((UINT64)off<<32),FALSE,&v)) || !v || v==0xffffffff)return FALSE;
        if(off==T.vsec){if((v&0xfffff)!=0x1000b)return FALSE;vendor=TRUE;}
        if(off==T.rebar){if((v&0xfffff)!=0x10015)return FALSE;rebar=TRUE;}
        if(vendor && rebar)return TRUE;
        off=(v>>20)&0xfff; /* Capability links need not be in address order. */
    }
    return FALSE;
}
static BOOLEAN move_match(struct move_ops *io,const UINT32 *values) {
    BOOLEAN ok=TRUE;
    for(UINTN i=0;i<MOVE_REGISTERS;++i) {
        UINT32 v=0;
        if(EFI_ERROR(io->read(move_saved[i].address,move_saved[i].word,&v)) || v!=values[i])ok=FALSE;
    }
    return ok;
}
static BOOLEAN move_put_expected(struct move_ops *io,UINTN index,UINT32 v,UINT32 expected) {
    EFI_STATUS e=io->write(move_saved[index].address,move_saved[index].word,v);
    UINT32 got=0;
    EFI_STATUS read=io->read(move_saved[index].address,move_saved[index].word,&got);
    if(move_trace_count<64)move_trace[move_trace_count++]=(struct move_trace){move_saved[index].address,v,e,read,got,expected};
    return !EFI_ERROR(e) && !EFI_ERROR(read) && got==expected;
}
static BOOLEAN move_put(struct move_ops *io,UINTN index,UINT32 v) {
    return move_put_expected(io,index,v,v);
}
static BOOLEAN resize_probe(struct move_ops *io,BOOLEAN large) {
    /* Standard 64-bit BAR sizing probe while both decoders are off. Do not
     * enable decode at the probe address. Caller always programs a real base. */
    UINT64 mask=~((large?(1ULL<<35):T.len1)-1);
    if(!move_put_expected(io,3,0xffffffff,(UINT32)(mask>>32)))return FALSE;
    return move_put_expected(io,2,0xffffffff,((UINT32)mask&0xfffffff0U)|0xc);
}
static UINT64 move_hash(const UINT32 *p) {
    UINT64 h=14695981039346656037ULL;
    const UINT8 *bytes=(const void *)p;
    for(UINTN i=0;i<sizeof(move_original);++i){h^=bytes[i];h*=1099511628211ULL;}
    return h;
}
static BOOLEAN move_pattern(const UINT32 *p) {
    /* Uniform black or all-ones pages cannot establish correct device routing. */
    for(UINTN i=1;i<MOVE_SAMPLE_WORDS;++i)if(p[i]!=p[0])return TRUE;
    return FALSE;
}
static BOOLEAN move_aux_safe(UINT64 dev) {
    /* An enabled companion function (HDMI audio) may stay enabled only if none of
     * its memory BARs is prefetchable or inside the window that gets moved. */
    UINT64 first=MOVE_OLD_BAR1<MOVE_OLD_BAR3?MOVE_OLD_BAR1:MOVE_OLD_BAR3;
    UINT64 last=MOVE_OLD_BAR1+T.len1>MOVE_OLD_BAR3+T.len3?MOVE_OLD_BAR1+T.len1-1:MOVE_OLD_BAR3+T.len3-1;
    for(UINT32 r=0x10;r<=0x24;r+=4) {
        UINT32 lo=0,hi=0;if(EFI_ERROR(move_read(dev+r,FALSE,&lo)) || lo==0xffffffff)return FALSE;
        if(lo&1)continue;
        if(lo&8)return FALSE;
        UINT64 a=lo&~0xfULL;
        if((lo&6)==4){if(r==0x24 || EFI_ERROR(move_read(dev+r+4,FALSE,&hi)))return FALSE;a|=(UINT64)hi<<32;r+=4;}
        if(a && a>=first && a<=last)return FALSE;
    }
    return TRUE;
}
static BOOLEAN move_prepare(void) {
    move_prepared=FALSE;
    if(!gpu_root || !gpu_root->Mem.Read || !gpu_root->Pci.Write || !bs->RaiseTPL || !bs->RestoreTPL ||
       !display_capture_ok || T.layout_n!=8)return FALSE;
    /* Exactly one firmware framebuffer in BAR1 (the one found at discovery); any
     * other display (integrated graphics) must stay clear of the moved window. */
    UINTN in_bar1=0;
    for(UINTN j=0;j<display_range_count;++j) {
        const struct blocked_range *r=&display_ranges[j];
        if(r->base>=MOVE_OLD_BAR1 && r->base-MOVE_OLD_BAR1<T.len1) {
            if(r->base!=T.fb_base || r->length<MOVE_SCAN_BYTES || r->length>T.len1-(r->base-MOVE_OLD_BAR1))return FALSE;
            ++in_bar1;
        } else if(r->base<MOVE_OLD_BAR3+T.len3 && r->base+r->length>MOVE_OLD_BAR1)return FALSE;
    }
    if(in_bar1!=1)return FALSE;
    UINT32 v;
    /* Same upstream bridge, bus range and GPU BAR0/BAR5 as captured at discovery. */
    const UINT64 extra_addresses[]={MOVE_BRIDGE,MOVE_BRIDGE+8,MOVE_BRIDGE+0x18,MOVE_GPU+0x10,MOVE_GPU+0x24};
    const UINT32 extra_values[]={T.bridge_id,T.bridge_class,T.bridge_bus,T.bar0_reg,T.bar5_reg};
    for(UINTN i=0;i<5;++i) {
        if(EFI_ERROR(move_read(extra_addresses[i],FALSE,&v)) || v!=extra_values[i])return FALSE;
    }
    for(UINTN i=0;i<MOVE_REGISTERS;++i) {
        if(EFI_ERROR(move_read(move_saved[i].address,move_saved[i].word,&v)) || v!=T.orig[i])return FALSE;
        move_saved[i].value=v;
    }
    if(!resize_structure(&move_live))return FALSE;
    /* Only the GPU's own functions may sit behind the bridge whose window moves. */
    for(UINTN d=0;d<32;++d)for(UINTN f=0;f<8;++f) {
        if(d==0 && f==0)continue;
        UINT64 dev=MOVE_GPU|(d<<16)|(f<<8);
        if(EFI_ERROR(move_read(dev,FALSE,&v)))return FALSE;
        if((v&0xffff)==0xffff)continue;
        if(d || (v&0xffff)!=0x10de)return FALSE;
        UINT32 cmd=0;if(EFI_ERROR(move_read(dev+4,TRUE,&cmd)))return FALSE;
        if((cmd&6) && !move_aux_safe(dev))return FALSE;
    }
    move_prepared=TRUE;return TRUE;
}

/* Called only while GCD owns the entire 32 GiB + 32 MiB destination range.
 * Caller masks firmware event callbacks for this short transaction. No file,
 * console, pool, GCD or GOP calls occur here. No framebuffer bytes are written.
 * Tests inject errors at each PCI write, including partial writes that fail.
 */
static int move_transaction(struct move_ops *io,UINT64 base) {
    move_started=FALSE;move_registers_restored=FALSE;move_pixels_restored=FALSE;move_trace_count=0;
    move_initial_read=move_destination_read=move_restored_read=EFI_NOT_STARTED;
    move_original_hash=move_destination_hash=move_restored_hash=0;
    resize_supported=resize_restored_supported=0;
    resize_mask_verified=resize_original_mask_verified=FALSE;
    move_retained=FALSE;
    if(!move_prepared || !base || (base&((1ULL<<35)-1)) || base>APERTURE_LAST)return MOVE_DECLINED;
    UINT64 previous_new=MOVE_NEW_BAR1;MOVE_NEW_BAR1=base;
    UINT32 original[MOVE_REGISTERS];for(UINTN i=0;i<MOVE_REGISTERS;++i)original[i]=move_saved[i].value;
    if(!move_match(io,original) || !resize_structure(io)){MOVE_NEW_BAR1=previous_new;return MOVE_DECLINED;}
    BOOLEAN found=FALSE;
    for(move_sample_offset=0;move_sample_offset<MOVE_SCAN_BYTES;move_sample_offset+=sizeof(move_original)) {
        move_initial_read=io->sample(T.fb_base+move_sample_offset,move_original);
        if(EFI_ERROR(move_initial_read))return MOVE_DECLINED;
        if(move_pattern(move_original)){found=TRUE;break;}
    }
    if(!found)return MOVE_DECLINED;
    move_original_hash=move_hash(move_original);
    UINT64 bar3=base+(1ULL<<35),last=bar3+T.len3-1;
    UINT32 target[MOVE_REGISTERS];memcpy(target,original,sizeof(target));
    target[0]=original[0]&~4U;target[1]=move_commit?original[1]:(original[1]&~4U);
    target[2]=(UINT32)base|0xc;target[3]=(UINT32)(base>>32);
    target[4]=(UINT32)bar3|0xc;target[5]=(UINT32)(bar3>>32);
    target[6]=((UINT32)(base>>16)&0xfff0)|1|((UINT32)last&0xfff00000)|0x10000;
    target[7]=(UINT32)(base>>32);target[8]=(UINT32)(last>>32);
    target[9]=(original[9]&~0xf80U)|0xc80U;
    target[10]=(original[10]&~0x3f00U)|(REBAR_32G_INDEX<<8);
    memcpy(move_target,target,sizeof(move_target));
    BOOLEAN moved=FALSE,operation_ok=TRUE;
    move_started=TRUE; /* Set before first write: a failing write may still act. */
    if(!move_put(io,0,original[0]&~6U))operation_ok=FALSE;
    if(!move_put(io,1,original[1]&~6U))operation_ok=FALSE;
    if(operation_ok && !move_put(io,9,target[9]))operation_ok=FALSE;
    if(operation_ok && (EFI_ERROR(io->read(RESIZE_CAP,FALSE,&resize_supported)) ||
        (resize_supported&T.cap1)!=T.cap1 || !(resize_supported&(1U<<(4+REBAR_32G_INDEX)))))operation_ok=FALSE;
    if(operation_ok && !move_put(io,10,target[10]))operation_ok=FALSE;
    if(operation_ok) {
        resize_mask_verified=resize_probe(io,TRUE);
        if(!resize_mask_verified)operation_ok=FALSE;
    }
    if(operation_ok) {
        /* Decode is off. Write upper halves before lower halves. */
        const UINTN order[]={3,2,5,4,7,8,6};
        for(UINTN i=0;i<7;++i)if(!move_put(io,order[i],target[order[i]])){operation_ok=FALSE;break;}
    }
    if(operation_ok && !move_put(io,1,target[1]))operation_ok=FALSE;
    if(operation_ok && !move_put(io,0,target[0]))operation_ok=FALSE;
    if(operation_ok && move_match(io,target)) {
        move_destination_read=io->sample(base+(T.fb_base-MOVE_OLD_BAR1)+move_sample_offset,move_check);
        if(!EFI_ERROR(move_destination_read)) {
            move_destination_hash=move_hash(move_check);
            moved=same(move_original,move_check,sizeof(move_original));
        }
    }

    if(moved && move_commit && move_commit(base)) {move_retained=TRUE;return MOVE_PASS;}

    /* Every failed/uncommitted path restores addresses before enabling decode.
     * Do not short-circuit restoration after a write error; verify final state.
     * If disabling decode cannot be verified, leave the reservation held and halt.
     */
    BOOLEAN off_gpu=move_put(io,0,original[0]&~6U);
    BOOLEAN off_bridge=move_put(io,1,original[1]&~6U);
    if(!off_gpu || !off_bridge)return MOVE_UNSAFE;
    /* Shrink BEFORE restoring the old base, which is not 32 GiB aligned.
     * Keep the override armed until the 256 MiB selector is verified. */
    if(!move_put(io,10,original[10]))return MOVE_UNSAFE;
    if(!move_put(io,9,original[9]))return MOVE_UNSAFE;
    if(EFI_ERROR(io->read(RESIZE_CAP,FALSE,&resize_restored_supported)) || resize_restored_supported!=T.cap1)return MOVE_UNSAFE;
    resize_original_mask_verified=resize_probe(io,FALSE);
    if(!resize_original_mask_verified)return MOVE_UNSAFE;
    const UINTN restore_order[]={3,2,5,4,7,8,6};
    for(UINTN i=0;i<7;++i)(void)move_put(io,restore_order[i],original[restore_order[i]]);
    UINT32 disabled[MOVE_REGISTERS];memcpy(disabled,original,sizeof(disabled));
    disabled[0]&=~6U;disabled[1]&=~6U;
    if(!move_match(io,disabled))return MOVE_UNSAFE;
    (void)move_put(io,1,original[1]);(void)move_put(io,0,original[0]);
    move_registers_restored=move_match(io,original);
    if(!move_registers_restored)return MOVE_UNSAFE;
    move_restored_read=io->sample(T.fb_base+move_sample_offset,move_check);
    if(!EFI_ERROR(move_restored_read))move_restored_hash=move_hash(move_check);
    move_pixels_restored=!EFI_ERROR(move_restored_read) &&
        same(move_original,move_check,sizeof(move_original));
    if(!move_pixels_restored)return MOVE_UNSAFE;
    return moved?MOVE_PASS:MOVE_FAILED_RESTORED;
}
static int relocation_trial(UINT64 base) {
    hex("move.target-bar1",base);hex("move.target-bar3",base+(1ULL<<35));
    /* Printed on screen too (not only to the log): the move check needs visible,
     * non-uniform pixels in the framebuffer. A blank screen cannot prove the move. */
    BOOLEAN was_quiet=quiet;quiet=FALSE;
    emit("RESIZE_BEGIN. Resizing BAR1 to 32 GiB. If the screen freezes here, power off fully.");
    quiet=was_quiet;
    flush_log();if(!log_ok)return MOVE_DECLINED;
    EFI_TPL previous=bs->RaiseTPL(TPL_NOTIFY);
    move_outcome=move_transaction(&move_live,base);
    if(move_outcome==MOVE_UNSAFE) {
        /* Never return to console/event callbacks or Windows with unverified
         * resources. The pre-armed watchdog or manual power-off recovers. */
        for(;;)__asm__ volatile("pause");
    }
    bs->RestoreTPL(previous);
    hex("move.result",move_outcome);hex("move.started",move_started);
    hex("handoff.large-bar-retained",move_retained);
    hex("resize.supported-after-override",resize_supported);
    hex("resize.32g-mask-verified",resize_mask_verified);
    hex("resize.restored-supported",resize_restored_supported);
    hex("resize.original-256m-mask-verified",resize_original_mask_verified);
    hex("move.sample-offset",move_sample_offset);hex("move.sample-bytes",sizeof(move_original));
    hex("move.registers-restored",move_registers_restored);hex("move.pixels-restored",move_pixels_restored);
    hex("move.initial-read-status",move_initial_read);hex("move.destination-read-status",move_destination_read);
    hex("move.restored-read-status",move_restored_read);
    hex("move.original-sample-hash",move_original_hash);hex("move.destination-sample-hash",move_destination_hash);
    hex("move.restored-sample-hash",move_restored_hash);
    for(UINTN i=0;i<move_trace_count;++i) {
        hex("move.write.address",move_trace[i].address);hex("move.write.value",move_trace[i].value);
        hex("move.write.status",move_trace[i].status);
        hex("move.write.read-status",move_trace[i].read_status);hex("move.write.readback",move_trace[i].readback);
        hex("move.write.expected",move_trace[i].expected);
    }
    return move_outcome;
}
