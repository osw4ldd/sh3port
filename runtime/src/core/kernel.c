/*
 * EE kernel HLE: threads (Windows fibers), semaphores, interrupt handlers,
 * alarms and the syscall table.
 *
 * Scheduling follows the EE kernel: strict priorities (lower number wins),
 * FIFO within a priority, no time slicing.  Thread switches only happen
 * inside syscalls and at event safe points, where all guest state is in the
 * thread's R5900 context, so a fiber switch is a complete context switch.
 */
#include "../internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_THREADS 256
#define MAX_SEMAS 256
#define MAX_HANDLERS 64
#define MAX_ALARMS 64

#define THS_RUN 0x01
#define THS_READY 0x02
#define THS_WAIT 0x04
#define THS_SUSPEND 0x08
#define THS_WAITSUSPEND 0x0C
#define THS_DORMANT 0x10

#define TSW_NONE 0
#define TSW_SLEEP 1
#define TSW_SEMA 2

/* Unlike the IOP kernel (which has KE_* codes), the EE kernel reports every
 * failure as -1; games test for exactly that (e.g. `while (PollSema(s) != -1)`). */
#define KE_OK 0
#define KE_ERROR (-1)
#define KE_ILLEGAL_THID (-1)
#define KE_UNKNOWN_THID (-1)
#define KE_UNKNOWN_SEMID (-1)
#define KE_DORMANT (-1)
#define KE_NOT_DORMANT (-1)
#define KE_NOT_WAIT (-1)
#define KE_RELEASE_WAIT (-1)
#define KE_SEMA_ZERO (-1)
#define KE_SEMA_OVF (-1)
#define KE_WAIT_DELETE (-1)

#define INTR_STACK_TOP 0x00070000u   /* kernel RAM, unused by user ELFs */

typedef struct ps2_thread {
    R5900 ctx;                /* first member: 16-byte aligned */
    int used;
    int id;
    int status;
    int wait_type, wait_id;
    int wait_result;
    int wakeup_count;
    int suspend_count;
    int init_prio, prio;
    uint32_t func, stack, stack_size, gp, attr, option, arg;
    int64_t seq;              /* FIFO order within a priority */
    void *fiber;
    int fresh;                /* fiber must (re)start at func */
} ps2_thread;

typedef struct sema {
    int used;
    int count, max_count, init_count;
    uint32_t attr, option;
    int waiters;
} sema;

typedef struct handler {
    int used;
    int id;
    int enabled;
    uint32_t func, arg, gp;
    int order;
} handler;

typedef struct alarm {
    int used;
    double deadline;
    uint32_t func, arg, gp;
    uint16_t time;
} alarm;

static ps2_thread *threads[MAX_THREADS];
static ps2_thread *cur;
static sema semas[MAX_SEMAS];
static handler intc_handlers[16][MAX_HANDLERS];
static handler dmac_handlers[16][MAX_HANDLERS];
static alarm alarms[MAX_ALARMS];
static int64_t seq_counter;
static int64_t front_counter;
static int next_handler_id = 1;
/* Syscall handler table in guest kernel RAM (see SetSyscall). */
#define SYSCALL_TABLE 0x80012000u
#define SYSCALL_TABLE_LEN 512u
static uint32_t heap_end;
static uint32_t main_stack_bottom;
static void *dead_fiber;
static uint32_t vsync_flag_addr, vsync_csr_addr;
static int need_resched;
static void *main_fiber;
static R5900 *hctx;           /* interrupt handler context */
int kernel_in_handler;

static void kernel_reschedule(void);
static void maybe_preempt(void);

/* Debugging aid for host-side I/O traces. */
int kernel_thread_id(void);

/* ---------------------------------------------------------- helpers */
static ps2_thread *thread_get(int id)
{
    if (id == 0)
        return cur;
    if (id < 0 || id >= MAX_THREADS || !threads[id] || !threads[id]->used)
        return NULL;
    return threads[id];
}

int kernel_thread_id(void)
{
    return kernel_in_handler ? -1 : (cur ? cur->id : 0);
}

static R5900 *alloc_ctx_thread(void)
{
    ps2_thread *t = _aligned_malloc(sizeof(ps2_thread), 64);
    memset(t, 0, sizeof *t);
    return &t->ctx;
}

static void init_ctx(R5900 *c)
{
    memset(c, 0, sizeof *c);
    c->vf[0].F[3] = 1.0f;
    c->vu_r = 0x3F800000u;
}

static void make_ready(ps2_thread *t, int front)
{
    t->status = THS_READY;
    t->seq = front ? --front_counter : ++seq_counter;
    if (t != cur && (!cur || t->prio < cur->prio))
        need_resched = 1;
}

static ps2_thread *pick_ready(void)
{
    ps2_thread *best = NULL;
    for (int i = 1; i < MAX_THREADS; i++) {
        ps2_thread *t = threads[i];
        if (!t || !t->used || t->status != THS_READY)
            continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq))
            best = t;
    }
    return best;
}

/* ---------------------------------------------------------- fibers */
static void CALLBACK thread_fiber(void *param)
{
    ps2_thread *t = param;
    for (;;) {
        R5900 *c = &t->ctx;
        if (dead_fiber) {
            DeleteFiber(dead_fiber);
            dead_fiber = NULL;
        }
        t->fresh = 0;
        c->r[4].SD[0] = (int32_t)t->arg;
        c->r[28].SD[0] = (int32_t)t->gp;
        c->r[29].SD[0] = (int32_t)((t->stack + t->stack_size - 0x40) & ~15u);
        c->r[31].SD[0] = 0;
        ps2_call(c, t->func);
        /* Returning from the entry function == ExitThread(). */
        t->status = THS_DORMANT;
        if (ps2_cfg.trace_syscalls)
            ps2_log("thread %d returned", t->id);
        kernel_reschedule();
    }
}

static void switch_to(ps2_thread *n)
{
    if (n == cur) {
        cur->status = THS_RUN;
        return;
    }
    ps2_thread *prev = cur;
    if (prev && prev->status == THS_RUN)
        make_ready(prev, 1);
    cur = n;
    n->status = THS_RUN;
    if (n->fresh && n->fiber && n->fiber != GetCurrentFiber()) {
        /* Restarted thread: discard the old fiber (it is suspended).  The
         * converted main fiber cannot be deleted; it simply stays parked. */
        if (n->fiber != main_fiber)
            DeleteFiber(n->fiber);
        n->fiber = NULL;
    }
    if (!n->fiber)
        n->fiber = CreateFiberEx(64 * 1024, 16 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH,
                                 thread_fiber, n);
    if (!n->fiber)
        ps2_fatal("CreateFiber failed (%lu)", GetLastError());
    SwitchToFiber(n->fiber);
    /* Resumed. */
    if (dead_fiber && dead_fiber != GetCurrentFiber()) {
        DeleteFiber(dead_fiber);
        dead_fiber = NULL;
    }
}

/* Pick the next thread to run.  Called when the current thread blocked,
 * exited, or a higher-priority thread may be ready. */
static void kernel_reschedule(void)
{
    double idle_start = -1;
    need_resched = 0;
    for (;;) {
        ps2_thread *n = pick_ready();
        if (cur && cur->status == THS_RUN) {
            if (!n || n->prio >= cur->prio)
                return;
        }
        if (n) {
            switch_to(n);
            return;
        }
        int alive = 0;
        for (int i = 1; i < MAX_THREADS && !alive; i++)
            alive = threads[i] && threads[i]->used && threads[i]->status != THS_DORMANT;
        if (!alive) {
            if (ps2_verbose)
                ps2_log("no threads left, exiting");
            kernel_exit(0);
        }
        if (idle_start < 0)
            idle_start = ps2_host_time();
        else if (ps2_host_time() - idle_start > 10.0) {
            ps2_warn_once("deadlock", "all threads have been blocked for 10 seconds");
        }
        kernel_idle_wait();
    }
}

static void maybe_preempt(void)
{
    if (kernel_in_handler || !need_resched)
        return;
    kernel_reschedule();
}

static void block_current(int wait_type, int wait_id)
{
    cur->status = THS_WAIT;
    cur->wait_type = wait_type;
    cur->wait_id = wait_id;
    cur->wait_result = KE_OK;
    kernel_reschedule();
}

/* ------------------------------------------------------- interrupts */
static void call_handler(handler *h, uint32_t a0, uint32_t a2)
{
    R5900 *c = hctx;
    int prev = kernel_in_handler;
    kernel_in_handler = 1;
    c->r[4].SD[0] = (int32_t)a0;
    c->r[5].SD[0] = (int32_t)h->arg;
    c->r[6].SD[0] = (int32_t)a2;
    c->r[7].SD[0] = 0;
    c->r[28].SD[0] = (int32_t)h->gp;
    c->r[29].SD[0] = (int32_t)(INTR_STACK_TOP - 0x100);
    c->r[31].SD[0] = 0;
    ps2_call(c, h->func);
    kernel_in_handler = prev;
}

static void run_handlers(handler *list, int cause)
{
    /* Called in registration order ("next" = -1 appends). */
    int order[MAX_HANDLERS], n = 0;
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (list[i].used && list[i].enabled)
            order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && list[order[j]].order < list[order[j - 1]].order; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        handler *h = &list[order[i]];
        if (!h->used || !h->enabled)
            continue;
        call_handler(h, (uint32_t)cause, 0);
        /* A negative return value stops the chain. */
        if (hctx->r[2].SW[0] < 0)
            break;
    }
}

static void fire_alarms(void)
{
    double now = ps2_host_time();
    for (int i = 0; i < MAX_ALARMS; i++) {
        alarm *a = &alarms[i];
        if (!a->used || a->deadline > now)
            continue;
        a->used = 0;
        handler h = {1, i, 1, a->func, a->arg, a->gp, 0};
        R5900 *c = hctx;
        int prev = kernel_in_handler;
        kernel_in_handler = 1;
        c->r[4].SD[0] = i;
        c->r[5].SD[0] = a->time;
        c->r[6].SD[0] = (int32_t)a->arg;
        c->r[28].SD[0] = (int32_t)h.gp;
        c->r[29].SD[0] = (int32_t)(INTR_STACK_TOP - 0x100);
        ps2_call(c, h.func);
        kernel_in_handler = prev;
    }
}

double kernel_next_alarm(void)
{
    double t = 1e30;
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && alarms[i].deadline < t)
            t = alarms[i].deadline;
    return t;
}

static uint64_t vblank_count;
static int frame_dumped;

static void apply_patches(void)
{
    for (uint32_t i = 0; i < ps2_patch_count; i++) {
        const ps2_patch *p = &ps2_patches[i];
        if (p->option >= 0 && !ps2_opt_flags[p->option])
            continue;
        if (p->cond_addr && ps2_read16(p->cond_addr) != (uint16_t)p->cond_value)
            continue;
        switch (p->size) {
        case 1: ps2_write8(p->addr, (uint8_t)p->value); break;
        case 2: ps2_write16(p->addr, (uint16_t)p->value); break;
        default: ps2_write32(p->addr, p->value); break;
        }
    }
}

void kernel_on_vblank(void)
{
    apply_patches();
    if (vsync_flag_addr) {
        ps2_write32(vsync_flag_addr, 1);
        if (vsync_csr_addr)
            ps2_write64(vsync_csr_addr, be_gs_csr());
    }
    vblank_count++;
    /* Request the frame dump a few vblanks early: hardware renderers capture
     * it asynchronously at a later vsync. */
    static int dump_lead;
    if (!dump_lead) { /* PS2NATIVE_GSDUMP=N (debugging): leave time for an N-frame GS dump */
        const char *env = getenv("PS2NATIVE_GSDUMP");
        dump_lead = 3 + (env ? atoi(env) : 0);
    }
    if (ps2_cfg.dump_frame[0] && ps2_cfg.max_frames && !frame_dumped &&
        vblank_count + (uint64_t)dump_lead >= (uint64_t)ps2_cfg.max_frames) {
        frame_dumped = 1;
        be_save_frame(ps2_cfg.dump_frame);
    }
    if (ps2_cfg.max_frames && vblank_count >= (uint64_t)ps2_cfg.max_frames) {
        if (ps2_verbose)
            ps2_log("frame limit reached");
        kernel_exit(0);
    }
}

void kernel_deliver_interrupts(R5900 *c)
{
    (void)c;
    if (!kernel_interrupts_enabled())
        return;
    for (int guard = 0; guard < 8; guard++) {
        uint32_t pend = be_intc_pending();
        uint32_t dpend = be_dmac_pending();
        int any = 0;
        if (dpend) {
            for (int ch = 0; ch < 10; ch++) {
                if (!(dpend & (1u << ch)))
                    continue;
                be_dmac_ack(1u << ch);
                run_handlers(dmac_handlers[ch], ch);
                any = 1;
            }
        }
        if (pend) {
            for (int cause = 0; cause < 16; cause++) {
                if (!(pend & (1u << cause)))
                    continue;
                be_intc_ack(1u << cause);
                run_handlers(intc_handlers[cause], cause);
                any = 1;
            }
        }
        if (!any)
            break;
    }
    fire_alarms();
}

volatile int ps2_event_flag;

/* --trace sampling profiler: every service call (~1 kHz while guest code
 * runs) records where the guest was (the backward branch that polled). */
#define PROF_SLOTS 512
static uint32_t prof_pc[PROF_SLOTS], prof_n[PROF_SLOTS], prof_total;
static uint8_t prof_tid[PROF_SLOTS];

static void prof_sample(uint32_t pc, int tid)
{
    uint32_t h = (pc >> 2) * 2654435761u;
    for (int i = 0; i < PROF_SLOTS; i++) {
        uint32_t s = (h + i) % PROF_SLOTS;
        if (prof_n[s] && (prof_pc[s] != pc || prof_tid[s] != tid))
            continue;
        prof_pc[s] = pc;
        prof_tid[s] = (uint8_t)tid;
        prof_n[s]++;
        prof_total++;
        return;
    }
}

static void prof_report(void)
{
    if (!prof_total)
        return;
    char line[512];
    int len = 0;
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < PROF_SLOTS; i++)
            if (prof_n[i] && (best < 0 || prof_n[i] > prof_n[best]))
                best = i;
        if (best < 0)
            break;
        len += snprintf(line + len, sizeof line - len, " t%d:%08X %u%%", prof_tid[best], prof_pc[best],
                        prof_n[best] * 100 / prof_total);
        prof_n[best] = 0;
    }
    ps2_log("profile (%u samples):%s", prof_total, line);
    memset(prof_n, 0, sizeof prof_n);
    prof_total = 0;
}

void ps2_service(R5900 *c)
{
    ps2_event_flag = 0;
    if (ps2_cfg.trace_syscalls)
        prof_sample(c->pc, cur ? cur->id : 0);
    be_update();
    if (!be_iop_lle())
        sif_poll();
    if (ps2_cfg.trace_syscalls) {
        static double last_report;
        double now = ps2_host_time();
        if (now - last_report > 2.0) {
            last_report = now;
            prof_report();
            ps2_log("state: thread %d at %08X (ra %08X) status %08X intc pending %08X dmac pending %03X handler %d",
                    cur ? cur->id : -1, cur ? cur->ctx.pc : 0, cur ? cur->ctx.r[31].UW[0] : 0, ps2_cop0[12], be_intc_pending(),
                    be_dmac_pending(), kernel_in_handler);
            /* PS2NATIVE_PEEK=hex,hex: guest words printed with every report */
            const char *pk = getenv("PS2NATIVE_PEEK");
            if (pk) {
                char line[256] = "";
                int len = 0;
                while (*pk && len < (int)sizeof line - 24) {
                    char *end;
                    unsigned long a = strtoul(pk, &end, 16);
                    if (end == pk)
                        break;
                    len += snprintf(line + len, sizeof line - len, " [%08lX]=%08X", a, ps2_read32((uint32_t)a));
                    pk = *end ? end + 1 : end;
                }
                ps2_log("peek:%s", line);
            }
            be_report();
            if (!be_iop_lle())
                sif_rpc_report(0);
            for (int i = 1; i < MAX_THREADS; i++) {
                ps2_thread *t = threads[i];
                if (!t || !t->used)
                    continue;
                ps2_log("  thread %2d prio %3d status %02X wait %d/%d wakeups %d suspend %d func %08X",
                        t->id, t->prio, t->status, t->wait_type, t->wait_id, t->wakeup_count,
                        t->suspend_count, t->func);
            }
        }
    }
    kernel_deliver_interrupts(c);
    if (be_should_quit())
        kernel_exit(0);
    maybe_preempt();
}

void kernel_idle_wait(void)
{
    if (ps2_cfg.trace_syscalls && ps2_event_flag) {
        ps2_event_flag = 0;
        prof_sample(0xFFFFFFFFu, 0);   /* all threads blocked */
    }
    double next = be_next_event_time();
    double na = kernel_next_alarm();
    double now = ps2_host_time();
    if (na < next)
        next = na;
    if (next > now) {
        double ms = (next - now) * 1000.0;
        if (ms > 1.0)
            Sleep(ms > 16 ? 16 : (DWORD)ms);
    }
    be_update();
    if (!be_iop_lle())
        sif_poll();
    int saved = kernel_in_handler;
    kernel_in_handler = 0;
    kernel_deliver_interrupts(hctx);
    kernel_in_handler = saved;
    if (be_should_quit())
        kernel_exit(0);
}

/* ------------------------------------------------------- semaphores */
static int sema_create(uint32_t p)
{
    for (int i = 1; i < MAX_SEMAS; i++) {
        if (semas[i].used)
            continue;
        sema *s = &semas[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->max_count = (int32_t)ps2_read32(p + 4);
        s->init_count = (int32_t)ps2_read32(p + 8);
        s->count = s->init_count;
        s->attr = ps2_read32(p + 16);
        s->option = ps2_read32(p + 20);
        return i;
    }
    return KE_ERROR;
}

static sema *sema_get(int id)
{
    if (id <= 0 || id >= MAX_SEMAS || !semas[id].used)
        return NULL;
    return &semas[id];
}

static ps2_thread *first_waiter(int type, int id)
{
    ps2_thread *best = NULL;
    for (int i = 1; i < MAX_THREADS; i++) {
        ps2_thread *t = threads[i];
        if (!t || !t->used || !(t->status & THS_WAIT) || t->wait_type != type || t->wait_id != id)
            continue;
        if (!best || t->seq < best->seq)
            best = t;
    }
    return best;
}

static void release_wait(ps2_thread *t, int result)
{
    t->wait_result = result;
    t->wait_type = TSW_NONE;
    if (t->status == THS_WAITSUSPEND)
        t->status = THS_SUSPEND;
    else
        make_ready(t, 0);
}

static int sema_signal(int id)
{
    sema *s = sema_get(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    ps2_thread *w = first_waiter(TSW_SEMA, id);
    if (w) {
        s->waiters--;
        release_wait(w, id);
        return KE_OK;
    }
    if (s->max_count > 0 && s->count >= s->max_count)
        return KE_SEMA_OVF;
    s->count++;
    return KE_OK;
}

static int sema_wait(int id)
{
    sema *s = sema_get(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    if (s->count > 0) {
        s->count--;
        return id;
    }
    if (kernel_in_handler)
        return KE_SEMA_ZERO;
    s->waiters++;
    cur->seq = ++seq_counter;
    block_current(TSW_SEMA, id);
    return cur->wait_result == KE_OK ? id : cur->wait_result;
}

static int sema_delete(int id)
{
    sema *s = sema_get(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    ps2_thread *w;
    while ((w = first_waiter(TSW_SEMA, id)) != NULL)
        release_wait(w, KE_WAIT_DELETE);
    s->used = 0;
    return KE_OK;
}

/* ---------------------------------------------------------- threads */
static int thread_create(uint32_t p)
{
    for (int i = 2; i < MAX_THREADS; i++) {
        if (threads[i] && threads[i]->used)
            continue;
        ps2_thread *t = threads[i];
        if (!t) {
            t = (ps2_thread *)alloc_ctx_thread();
            threads[i] = t;
        } else {
            void *f = t->fiber;
            memset(t, 0, sizeof *t);
            t->fiber = f;
        }
        init_ctx(&t->ctx);
        t->ctx.thread = (struct ps2_thread *)t;
        t->used = 1;
        t->id = i;
        t->status = THS_DORMANT;
        t->func = ps2_read32(p + 4);
        t->stack = ps2_read32(p + 8);
        t->stack_size = ps2_read32(p + 12);
        t->gp = ps2_read32(p + 16);
        t->init_prio = (int32_t)ps2_read32(p + 20);
        t->prio = t->init_prio;
        t->attr = ps2_read32(p + 28);
        t->option = ps2_read32(p + 32);
        t->fresh = 1;
        return i;
    }
    return KE_ERROR;
}

static int thread_start(int id, uint32_t arg)
{
    ps2_thread *t = thread_get(id);
    if (!t || t == cur)
        return KE_ILLEGAL_THID;
    if (t->status != THS_DORMANT)
        return KE_NOT_DORMANT;
    t->arg = arg;
    t->prio = t->init_prio;
    t->wakeup_count = 0;
    t->fresh = 1;
    make_ready(t, 0);
    return id;
}

static void thread_exit_current(int delete_it)
{
    ps2_thread *t = cur;
    if (kernel_in_handler) {
        ps2_warn_once("exitinhandler", "ExitThread called from an interrupt handler");
        return;
    }
    t->status = THS_DORMANT;
    t->fresh = 1;
    if (delete_it)
        t->used = 0;
    /* The fiber we are running on is recycled when the thread restarts;
     * a restart from another fiber deletes it. */
    kernel_reschedule();
    /* Only reached if this thread was restarted while still on its fiber:
     * unwind by starting over at the entry point. */
    ps2_fatal("thread %d resumed after exit", t->id);
}

static int thread_terminate(int id)
{
    ps2_thread *t = thread_get(id);
    if (!t || t == cur)
        return KE_ILLEGAL_THID;
    if (t->status == THS_DORMANT)
        return KE_DORMANT;
    t->status = THS_DORMANT;
    t->fresh = 1;
    return KE_OK;
}

static int thread_change_prio(int id, int prio)
{
    ps2_thread *t = thread_get(id);
    if (!t)
        return KE_UNKNOWN_THID;
    int old = t->prio;
    t->prio = prio;
    if (t == cur) {
        if (prio > old)
            need_resched = 1;
    } else if (t->status == THS_READY) {
        t->seq = ++seq_counter;
        if (cur && prio < cur->prio)
            need_resched = 1;
    }
    return old;
}

static int thread_rotate(int prio)
{
    if (cur && cur->prio == prio) {
        ps2_thread *n = pick_ready();
        if (n && n->prio == prio) {
            cur->status = THS_READY;
            cur->seq = ++seq_counter;
            need_resched = 1;
        }
        return KE_OK;
    }
    ps2_thread *n = pick_ready();
    if (n && n->prio == prio)
        n->seq = ++seq_counter;
    return KE_OK;
}

static int thread_refer(int id, uint32_t p)
{
    ps2_thread *t = thread_get(id);
    /* The kernel returns the status of the thread slot: 0 once its thread
     * is gone (games poll for a worker thread to disappear). */
    if (!t)
        return (id > 0 && id < MAX_THREADS) ? 0 : KE_UNKNOWN_THID;
    if (p) {
        ps2_write32(p + 0x00, (uint32_t)t->status);
        ps2_write32(p + 0x04, t->func);
        ps2_write32(p + 0x08, t->stack);
        ps2_write32(p + 0x0C, t->stack_size);
        ps2_write32(p + 0x10, t->gp);
        ps2_write32(p + 0x14, (uint32_t)t->init_prio);
        ps2_write32(p + 0x18, (uint32_t)t->prio);
        ps2_write32(p + 0x1C, t->attr);
        ps2_write32(p + 0x20, t->option);
        ps2_write32(p + 0x24, (uint32_t)t->wait_type);
        ps2_write32(p + 0x28, (uint32_t)t->wait_id);
        ps2_write32(p + 0x2C, (uint32_t)t->wakeup_count);
    }
    return t->status;
}

static int thread_sleep(void)
{
    if (cur->wakeup_count > 0) {
        cur->wakeup_count--;
        return cur->id;
    }
    if (kernel_in_handler)
        return KE_ERROR;
    cur->seq = ++seq_counter;
    block_current(TSW_SLEEP, 0);
    return cur->wait_result == KE_OK ? cur->id : cur->wait_result;
}

static int thread_wakeup(int id)
{
    ps2_thread *t = thread_get(id);
    if (!t || (t == cur && !kernel_in_handler))
        return KE_ILLEGAL_THID;
    if (t->status == THS_DORMANT)
        return KE_DORMANT;
    if ((t->status & THS_WAIT) && t->wait_type == TSW_SLEEP)
        release_wait(t, KE_OK);
    else
        t->wakeup_count++;
    return id;
}

static int thread_suspend(int id)
{
    ps2_thread *t = thread_get(id);
    if (!t)
        return KE_UNKNOWN_THID;
    if (t->status == THS_DORMANT)
        return KE_DORMANT;
    t->suspend_count++;
    if (t->status == THS_READY)
        t->status = THS_SUSPEND;
    else if (t->status == THS_WAIT)
        t->status = THS_WAITSUSPEND;
    else if (t == cur && !kernel_in_handler) {
        t->status = THS_SUSPEND;
        kernel_reschedule();
    }
    return id;
}

static int thread_resume(int id)
{
    ps2_thread *t = thread_get(id);
    if (!t)
        return KE_UNKNOWN_THID;
    if (t->suspend_count == 0)
        return KE_ERROR;
    if (--t->suspend_count == 0) {
        if (t->status == THS_SUSPEND)
            make_ready(t, 0);
        else if (t->status == THS_WAITSUSPEND)
            t->status = THS_WAIT;
    }
    return id;
}

/* --------------------------------------------------------- handlers */
static int handler_add(handler *list, uint32_t func, int next, uint32_t arg, uint32_t gp)
{
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (list[i].used)
            continue;
        handler *h = &list[i];
        h->used = 1;
        h->enabled = 1;
        h->id = next_handler_id++;
        h->func = func;
        h->arg = arg;
        h->gp = gp;
        h->order = next == 0 ? -h->id : h->id;
        return h->id;
    }
    return KE_ERROR;
}

static handler *handler_find(handler *list, int id)
{
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (list[i].used && list[i].id == id)
            return &list[i];
    return NULL;
}

static int set_alarm(uint16_t time, uint32_t func, uint32_t arg, uint32_t gp)
{
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (alarms[i].used)
            continue;
        alarms[i].used = 1;
        alarms[i].time = time;
        alarms[i].func = func;
        alarms[i].arg = arg;
        alarms[i].gp = gp;
        alarms[i].deadline = ps2_host_time() + time * (1.0 / 15734.0);
        ps2_event_flag = 1;
        return i;
    }
    return KE_ERROR;
}

/* ---------------------------------------------------------- syscalls */
static uint32_t osd_config_word(void)
{
    /* English, 4:3, RGB/YCbCr off, version 2. */
    return (1u << 16) | (2u << 13);
}

static void run_user_syscall(R5900 *c, uint32_t handler_addr)
{
    R5900 *tmp = _aligned_malloc(sizeof(R5900), 64);
    memcpy(tmp, c, sizeof *tmp);
    tmp->r[31].SD[0] = 0;
    ps2_call(tmp, handler_addr);
    c->r[2] = tmp->r[2];
    c->r[3] = tmp->r[3];
    _aligned_free(tmp);
}

static const char *syscall_name(int n)
{
    switch (n) {
    case 0x02: return "SetGsCrt";
    case 0x04: return "Exit";
    case 0x10: return "AddIntcHandler";
    case 0x12: return "AddDmacHandler";
    case 0x14: return "EnableIntc";
    case 0x16: return "EnableDmac";
    case 0x20: return "CreateThread";
    case 0x22: return "StartThread";
    case 0x29: return "ChangeThreadPriority";
    case 0x3C: return "SetupThread";
    case 0x3D: return "SetupHeap";
    case 0x40: return "CreateSema";
    case 0x42: return "SignalSema";
    case 0x44: return "WaitSema";
    case 0x64: return "FlushCache";
    case 0x71: return "GsPutIMR";
    case 0x74: return "SetSyscall";
    case 0x77: return "SifSetDma";
    case 0x79: return "SifSetReg";
    case 0x7A: return "SifGetReg";
    default: return "";
    }
}

void kernel_syscall(R5900 *c, int32_t num)
{
    int n = num < 0 ? -num : num;
    uint32_t a0 = c->r[4].UW[0], a1 = c->r[5].UW[0], a2 = c->r[6].UW[0], a3 = c->r[7].UW[0];
    int32_t ret = 0;

    if (ps2_cfg.trace_syscalls)
        ps2_log("syscall %d %s (%08X, %08X, %08X, %08X) thread %d%s pc %08X ra %08X", num,
                syscall_name(n), a0, a1, a2, a3, cur ? cur->id : -1,
                kernel_in_handler ? " [intr]" : "", c->pc, c->r[31].UW[0]);

    uint32_t user_handler = n < SYSCALL_TABLE_LEN ? ps2_read32(SYSCALL_TABLE + (uint32_t)n * 4) : 0;
    /* Handlers copied into kernel RAM (ps2sdk-style kernel patches) are not
     * recompiled: those syscalls keep their HLE implementation. */
    if (user_handler && user_handler < 0x80000000u && ps2_lookup(user_handler, NULL)) {
        run_user_syscall(c, user_handler);
        return;
    }

    switch (n) {
    case 0x01: /* ResetEE */
        break;
    case 0x02: /* SetGsCrt(interlace, mode, ffmd) */
        be_set_crt((int)a0, (int)a1, (int)a2);
        break;
    case 0x04: /* Exit */
        kernel_exit((int)a0);
    case 0x06: /* LoadExecPS2 */
    case 0x07: { /* ExecPS2 */
        char path[256] = "";
        if (n == 0x06)
            ps2_mem_readstr(a0, path, sizeof path);
        ps2_log("program requested %s %s - not supported, exiting",
                n == 6 ? "LoadExecPS2" : "ExecPS2", path);
        kernel_exit(0);
    }
    case 0x0D: case 0x0E: case 0x0F: /* SetV*Handler */
        break;
    case 0x10: /* AddIntcHandler(cause, handler, next, arg) */
        ret = handler_add(intc_handlers[a0 & 15], a1, (int32_t)a2, a3, c->r[28].UW[0]);
        break;
    case 0x11: { /* RemoveIntcHandler(cause, id) */
        /* Returns the id when a handler was removed, negative otherwise:
         * Konami's film player (Silent Hill 2/3) re-enables the VBlank
         * interrupt only when this is non-zero (as in ps2-native-recomp). */
        handler *h = handler_find(intc_handlers[a0 & 15], (int32_t)a1);
        if (h)
            h->used = 0;
        ret = h ? (int32_t)a1 : KE_ERROR;
        break;
    }
    case 0x12: /* AddDmacHandler(channel, handler, next, arg) */
        ret = handler_add(dmac_handlers[a0 & 15], a1, (int32_t)a2, a3, c->r[28].UW[0]);
        break;
    case 0x13: { /* RemoveDmacHandler(channel, id) */
        handler *h = handler_find(dmac_handlers[a0 & 15], (int32_t)a1);
        if (h)
            h->used = 0;
        ret = h ? (int32_t)a1 : KE_ERROR;
        break;
    }
    case 0x14: /* _EnableIntc */
        ret = !be_intc_set_mask((int)(a0 & 15), 1);
        break;
    case 0x15:
        ret = be_intc_set_mask((int)(a0 & 15), 0);
        break;
    case 0x16: /* _EnableDmac */
        ret = !be_dmac_set_mask((int)(a0 & 15), 1);
        break;
    case 0x17:
        ret = be_dmac_set_mask((int)(a0 & 15), 0);
        break;
    /* The library stubs use 0xFC in place of 0x18 (SetAlarm) and 0xFD in
     * place of 0x19 (ReleaseAlarm); the interrupt forms are 0xFE / 0xFF. */
    case 0x18: case 0x1E: case 0xFC: case 0xFE: /* SetAlarm / iSetAlarm */
        ret = set_alarm((uint16_t)a0, a1, a2, c->r[28].UW[0]);
        break;
    case 0x19: case 0x1F: case 0xFD: case 0xFF: /* ReleaseAlarm / iReleaseAlarm */
        if (a0 < MAX_ALARMS && alarms[a0].used) {
            alarms[a0].used = 0;
            ret = KE_OK;
        } else {
            ret = KE_ERROR;
        }
        break;
    case 0x1A: case 0x1C: /* _iEnableIntc / _iEnableDmac: same results as the thread forms */
        if (n == 0x1A)
            ret = !be_intc_set_mask((int)(a0 & 15), 1);
        else
            ret = !be_dmac_set_mask((int)(a0 & 15), 1);
        break;
    case 0x1B: case 0x1D:
        if (n == 0x1B)
            ret = be_intc_set_mask((int)(a0 & 15), 0);
        else
            ret = be_dmac_set_mask((int)(a0 & 15), 0);
        break;
    case 0x20: /* CreateThread */
        ret = thread_create(a0);
        break;
    case 0x21: { /* DeleteThread */
        ps2_thread *t = thread_get((int32_t)a0);
        if (!t || t == cur)
            ret = KE_ILLEGAL_THID;
        else if (t->status != THS_DORMANT)
            ret = KE_NOT_DORMANT;
        else
            t->used = 0;
        break;
    }
    case 0x22: /* StartThread */
        ret = thread_start((int32_t)a0, a1);
        break;
    case 0x23: /* ExitThread */
        thread_exit_current(0);
        break;
    case 0x24: /* ExitDeleteThread */
        thread_exit_current(1);
        break;
    case 0x25: case 0x26: /* TerminateThread */
        ret = thread_terminate((int32_t)a0);
        break;
    case 0x27: case 0x28: /* Disable/EnableDispatchThread */
        break;
    case 0x29: case 0x2A: /* ChangeThreadPriority */
        ret = thread_change_prio((int32_t)a0, (int32_t)a1);
        break;
    case 0x2B: case 0x2C: /* RotateThreadReadyQueue */
        ret = thread_rotate((int32_t)a0);
        break;
    case 0x2D: case 0x2E: { /* ReleaseWaitThread */
        ps2_thread *t = thread_get((int32_t)a0);
        if (!t)
            ret = KE_UNKNOWN_THID;
        else if (!(t->status & THS_WAIT))
            ret = KE_NOT_WAIT;
        else {
            if (t->wait_type == TSW_SEMA) {
                sema *s = sema_get(t->wait_id);
                if (s)
                    s->waiters--;
            }
            release_wait(t, KE_RELEASE_WAIT);
            ret = (int32_t)a0;
        }
        break;
    }
    case 0x2F: /* GetThreadId */
        ret = cur->id;
        break;
    case 0x30: case 0x31: /* ReferThreadStatus */
        ret = thread_refer((int32_t)a0, a1);
        break;
    case 0x32: /* SleepThread */
        ret = thread_sleep();
        break;
    case 0x33: case 0x34: /* WakeupThread */
        ret = thread_wakeup((int32_t)a0);
        break;
    case 0x35: case 0x36: { /* CancelWakeupThread */
        ps2_thread *t = thread_get((int32_t)a0);
        if (!t)
            ret = KE_UNKNOWN_THID;
        else {
            ret = t->wakeup_count;
            t->wakeup_count = 0;
        }
        break;
    }
    case 0x37: case 0x38: /* SuspendThread */
        ret = thread_suspend((int32_t)a0);
        break;
    case 0x39: case 0x3A: /* ResumeThread */
        ret = thread_resume((int32_t)a0);
        break;
    case 0x3C: { /* SetupThread(gp, stack, stack_size, args, root) */
        uint32_t stack = a1, size = a2;
        uint32_t top = (stack == 0xFFFFFFFFu) ? PS2_RAM_SIZE : stack + size;
        main_stack_bottom = top - size;
        cur->gp = a0;
        cur->stack = top - size;
        cur->stack_size = size;
        if (a3) {
            /* struct sargs { int argc; char *argv[16]; char payload[256]; } */
            char arg0[300];
            if (ps2_cfg.boot_path[0])
                snprintf(arg0, sizeof arg0, "%s", ps2_cfg.boot_path);
            else
                snprintf(arg0, sizeof arg0, "host:%s", ps2_cfg.elf_name);
            uint32_t len = (uint32_t)strlen(arg0) + 1;
            ps2_write32(a3, 1);
            ps2_write32(a3 + 4, a3 + 4 + 16 * 4);
            ps2_mem_write(a3 + 4 + 16 * 4, arg0, len);
            if (ps2_cfg.trace_syscalls)
                ps2_log("SetupThread: argv[0] = \"%s\" at %08X", arg0, a3 + 4 + 16 * 4);
        }
        ret = (int32_t)((top - 0x2A0) & ~15u);
        break;
    }
    case 0x3D: /* SetupHeap(start, size) */
        heap_end = ((int32_t)a1 == -1) ? main_stack_bottom : a0 + a1;
        ret = (int32_t)heap_end;
        break;
    case 0x3E: /* EndOfHeap */
        ret = (int32_t)heap_end;
        break;
    case 0x40: /* CreateSema */
        ret = sema_create(a0);
        break;
    case 0x41: case 0x49: /* DeleteSema */
        ret = sema_delete((int32_t)a0);
        break;
    case 0x42: case 0x43: /* SignalSema */
        ret = sema_signal((int32_t)a0);
        break;
    case 0x44: /* WaitSema */
        ret = sema_wait((int32_t)a0);
        break;
    case 0x45: case 0x46: { /* PollSema */
        sema *s = sema_get((int32_t)a0);
        if (!s)
            ret = KE_UNKNOWN_SEMID;
        else if (s->count > 0) {
            s->count--;
            ret = (int32_t)a0;
        } else
            ret = KE_SEMA_ZERO;
        break;
    }
    case 0x47: case 0x48: { /* ReferSemaStatus */
        sema *s = sema_get((int32_t)a0);
        if (!s) {
            ret = KE_UNKNOWN_SEMID;
            break;
        }
        ps2_write32(a1 + 0, (uint32_t)s->count);
        ps2_write32(a1 + 4, (uint32_t)s->max_count);
        ps2_write32(a1 + 8, (uint32_t)s->init_count);
        ps2_write32(a1 + 12, (uint32_t)s->waiters);
        ps2_write32(a1 + 16, s->attr);
        ps2_write32(a1 + 20, s->option);
        ret = (int32_t)a0;
        break;
    }
    case 0x4A: /* SetOsdConfigParam */
        break;
    case 0x4B: /* GetOsdConfigParam */
        ps2_write32(a0, osd_config_word());
        break;
    case 0x4C: case 0x4D: /* GetGsH/VParam */
        break;
    case 0x5A: /* Copy (kernel memcpy) */
        for (uint32_t i = 0; i + 4 <= a2; i += 4)
            ps2_write32(a0 + i, ps2_read32(a1 + i));
        break;
    case 0x5B: /* GetEntryAddress */
        ret = (int32_t)(0x80076000u + (uint32_t)a0 * 4);
        break;
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: { /* Enable/Disable Intc/Dmac handler */
        handler *list = (n <= 0x5D) ? intc_handlers[a0 & 15] : dmac_handlers[a0 & 15];
        handler *h = handler_find(list, (int32_t)a1);
        if (h)
            h->enabled = (n == 0x5C || n == 0x5E);
        ret = h ? KE_OK : KE_ERROR;
        break;
    }
    case 0x60: case 0x61: case 0x62: /* KSeg0 / EnableCache / DisableCache */
        break;
    case 0x63: case 0x67: /* GetCop0 */
        ret = (int32_t)ps2_cop0[a0 & 31];
        break;
    case 0x64: case 0x68: /* FlushCache */
        break;
    case 0x66: case 0x6A: /* CpuConfig */
        break;
    case 0x6B: /* SifStopDma */
        if (be_iop_lle())
            sif_lle_stop_dma();
        break;
    case 0x6C: case 0x6D: /* SetCPUTimerHandler / SetCPUTimer */
        ps2_warn_once("cputimer", "SetCPUTimer is not supported");
        break;
    case 0x6E: /* SetOsdConfigParam2 */
        break;
    case 0x6F: /* GetOsdConfigParam2(ptr, size, offset) */
        for (uint32_t i = 0; i < a1; i++)
            ps2_write8(a0 + i, 0);
        break;
    case 0x70: /* GsGetIMR */
        ret = (int32_t)be_gs_imr();
        break;
    case 0x71: /* GsPutIMR: returns the previous mask */
        ret = (int32_t)be_gs_imr();
        be_gs_set_imr(a0);
        break;
    case 0x73: /* SetVSyncFlag(flag, csr) */
        vsync_flag_addr = a0;
        vsync_csr_addr = a1;
        break;
    case 0x74: /* SetSyscall(num, handler) */
        /* The table lives in guest kernel RAM like on hardware: games locate
         * it (FindAddress) and patch entries directly. */
        if (a0 < SYSCALL_TABLE_LEN)
            ps2_write32(SYSCALL_TABLE + a0 * 4, a1);
        break;
    case 0x83: /* FindAddress(start, end, value): first word == value, or 0 */
        ret = 0;
        for (uint32_t p = a0 & ~3u; p + 4 <= a1; p += 4) {
            if (ps2_read32(p) == a2) {
                ret = (int32_t)p;
                break;
            }
        }
        break;
    case 0x75: { /* _print */
        char buf[512];
        ps2_mem_readstr(a0, buf, sizeof buf);
        fputs(buf, stdout);
        fflush(stdout);
        break;
    }
    case 0x76: /* SifDmaStat */
        ret = be_iop_lle() ? sif_lle_dma_stat((int32_t)a0) : -1;
        break;
    case 0x77: /* SifSetDma(transfers, count) */
        ret = be_iop_lle() ? sif_lle_set_dma(a0, (int32_t)a1) : sif_set_dma(a0, (int32_t)a1);
        break;
    case 0x78: /* SifSetDChain */
        if (be_iop_lle())
            sif_lle_dchain();
        else
            sif_dchain();
        break;
    case 0x79: /* SifSetReg */
        if (be_iop_lle())
            sif_lle_set_reg(a0, a1);
        else
            sif_set_reg(a0, a1);
        break;
    case 0x7A: /* SifGetReg */
        ret = (int32_t)(be_iop_lle() ? sif_lle_get_reg(a0) : sif_get_reg(a0));
        break;
    case 0x7B: /* ExecOSD */
        ps2_log("ExecOSD requested, exiting");
        kernel_exit(0);
    case 0x7C: /* Deci2Call */
        ret = -1;
        break;
    case 0x7D: /* PSMode */
        break;
    case 0x7E: /* MachineType */
        ret = 0;
        break;
    case 0x7F: /* GetMemorySize */
        ret = (int32_t)PS2_RAM_SIZE;
        break;
    case 0x80: /* _GetGsDxDyOffset */
        break;
    case 0x82: /* _InitTLB */
        break;
    default: {
        char key[32];
        snprintf(key, sizeof key, "sys%d", num);
        ps2_warn_once(key, "unimplemented syscall %d (0x%X) at %08X", num, n, c->pc);
        ret = KE_ERROR;
        break;
    }
    }
    c->r[2].SD[0] = ret;
    if (!kernel_in_handler)
        maybe_preempt();
}

/* ------------------------------------------------------------ setup */
void kernel_init(void)
{
    hctx = _aligned_malloc(sizeof(R5900), 64);
    init_ctx(hctx);
}

void kernel_exit(int code)
{
    fflush(stdout);
    if (ps2_cfg.dump_frame[0] && !frame_dumped)
        be_save_frame(ps2_cfg.dump_frame);
    if (ps2_verbose)
        ps2_log("guest exited with code %d", code);
    sif_rpc_report(1);
    be_shutdown(code);
    ExitProcess((UINT)code);
}

void kernel_run_main(uint32_t entry)
{
    main_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (!main_fiber)
        ps2_fatal("ConvertThreadToFiber failed");
    ps2_thread *t = (ps2_thread *)alloc_ctx_thread();
    init_ctx(&t->ctx);
    t->ctx.thread = (struct ps2_thread *)t;
    t->used = 1;
    t->id = 1;
    t->status = THS_RUN;
    t->prio = t->init_prio = 0;
    t->fiber = main_fiber;
    t->func = entry;
    threads[1] = t;
    cur = t;

    R5900 *c = &t->ctx;
    /* Before SetupThread runs, give the guest a usable stack anyway. */
    c->r[29].SD[0] = (int32_t)(PS2_RAM_SIZE - 0x1000);
    c->r[4].SD[0] = 0;   /* __start(struct sargs_start *pargs = NULL) */
    ps2_call(c, entry);
    /* _start normally ends in Exit(); falling off the end is an exit too. */
    kernel_exit((int)c->r[2].SW[0]);
}
