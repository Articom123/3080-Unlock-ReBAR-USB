/* SPDX-License-Identifier: MIT. Included by main.c before inventory.c.
 * Universal variant: every machine-specific constant of v0.6.1 is now a field of T,
 * discovered at boot (topo_discover). topo_defaults() reproduces the original
 * MSI B850 / RTX 3080 20GB values so the original host tests still describe it.
 */
#define GA102_ID 0x220610deU
#define VSEC_HEADER 0x02410001U
#define REBAR_32G_INDEX 15U

struct topology {
    UINT64 gpu,bridge,audio;               /* root-bridge PCI addresses: bus<<24|dev<<16|fn<<8 */
    UINT8 gpu_bus;
    UINT64 old_bar0,len0,old_bar1,len1,old_bar3,len3,new_bar1,fb_base;
    UINT64 ap_first,ap_last,reserve,scan;
    UINT32 subsys,vsec,rebar,bar1_cap,bar1_ctrl,idx1,cap1;
    UINT32 snap_rebar[7];                  /* ReBAR header + three cap/ctrl pairs for the snapshot */
    UINT32 bridge_id,bridge_class,bridge_bus,bar0_reg,bar5_reg;
    UINT32 orig[11];                       /* move_saved values captured at discovery */
    UINT32 layout_off[8],layout_val[8];
    UINTN layout_n;
    BOOLEAN already_large;
    const char *refusal;
};
static struct topology T;

#define MOVE_GPU T.gpu
#define MOVE_BRIDGE T.bridge
#define MOVE_AUDIO T.audio
#define MOVE_OLD_BAR1 T.old_bar1
#define MOVE_OLD_BAR3 T.old_bar3
#define MOVE_NEW_BAR1 T.new_bar1
#define RESIZE_OVERRIDE (MOVE_GPU|((UINT64)(T.vsec+0x10)<<32))
#define RESIZE_CONTROL (MOVE_GPU|((UINT64)T.bar1_ctrl<<32))
#define RESIZE_CAP (MOVE_GPU|((UINT64)T.bar1_cap<<32))
#define APERTURE_FIRST T.ap_first
#define APERTURE_LAST T.ap_last
#define RESERVATION_BYTES T.reserve

static void topo_apply(void);
static void topo_defaults(void) {
    struct topology z={0};T=z;
    T.gpu=1ULL<<24;T.bridge=(1ULL<<16)|(1ULL<<8);T.audio=T.gpu|(1ULL<<8);T.gpu_bus=1;
    T.old_bar0=0xde000000;T.len0=16ULL<<20;T.old_bar1=0xfcc0000000ULL;T.len1=256ULL<<20;
    T.old_bar3=0xfcd0000000ULL;T.len3=32ULL<<20;T.new_bar1=0x2000000000ULL;T.fb_base=T.old_bar1;
    T.ap_first=0x1860000000ULL;T.ap_last=0xfcffffffffULL;T.reserve=(1ULL<<35)+(32ULL<<20);T.scan=0x100000;
    T.subsys=0x146d10de;T.vsec=0x600;T.rebar=0xbb0;T.bar1_cap=0xbbc;T.bar1_ctrl=0xbc0;T.idx1=8;T.cap1=0x1c00;
    const UINT32 snap[]={0xbb0,0xbb4,0xbb8,0xbbc,0xbc0,0xbc4,0xbc8};
    for(UINTN i=0;i<7;++i)T.snap_rebar[i]=snap[i];
    T.bridge_id=0x14db1022;T.bridge_class=0x06040000;T.bridge_bus=0x00010100;T.bar0_reg=0xde000000;T.bar5_reg=0xf001;
    const UINT32 orig[]={3,7,0xc000000c,0xfc,0xd000000c,0xfc,0xd1f1c001,0xfc,0xfc,0x1001,0x801};
    for(UINTN i=0;i<11;++i)T.orig[i]=orig[i];
    const UINT32 lo[]={0x604,0x60c,0xbb0,0xbb4,0xbb8,0xbbc,0xbc4,0xbc8};
    const UINT32 lv[]={0x02410001,0x1801,0xc1c10015,0x100,0x460,0x1c00,0x200,0x503};
    for(UINTN i=0;i<8;++i){T.layout_off[i]=lo[i];T.layout_val[i]=lv[i];}
    T.layout_n=8;T.refusal="not checked";
    topo_apply();
}

