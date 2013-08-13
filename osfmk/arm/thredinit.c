/*
 * Copyright 2013, winocm. <rms@velocitylimitless.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 *   Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 *   Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 *   If you are going to use this software in any form that does not involve
 *   releasing the source to this project or improving it, let me know beforehand.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * ARM thread support
 */

/*
 * this is not smp safe at /all/.
 */

#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/debug.h>
#include <arm/cpu_data.h>
#include <kern/thread.h>
#include <arm/misc_protos.h>
#include <kern/kalloc.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

thread_t CurrentThread;

/* etimer is *still* broken and so are threads. :| */

#define kprintf(args...)
/*
 * These are defined in ctxswitch.s.
 */
thread_t Switch_context(thread_t old_thread, thread_continue_t continuation, thread_t new_thread);
void Call_continuation(thread_continue_t continuation, void *parameter, wait_result_t wresult);

static void save_vfp_context(thread_t thread);

/**
 * arm_set_threadpid_user_readonly
 *
 * Set current thread identifier to the cthread identifier.
 */
void arm_set_threadpid_user_readonly(uint32_t* address) {
    __asm__ __volatile__("mcr p15, 0, %0, c13, c0, 3" :: "r"(address));
}

/**
 * arm_set_threadpid_priv_readwrite
 *
 * Set current thread identifier to the specified thread_t.
 */
void arm_set_threadpid_priv_readwrite(uint32_t* address) {
    __asm__ __volatile__("mcr p15, 0, %0, c13, c0, 4" :: "r"(address));
}

/**
 * machine_set_current_thread
 *
 * Set current thread to the specified thread_t.
 */
void machine_set_current_thread(thread_t thread)
{
    /* Set the current thread. */
    CurrentThread = thread;
    
    arm_set_threadpid_user_readonly((uint32_t*)CurrentThread->machine.cthread_self);
    arm_set_threadpid_priv_readwrite((uint32_t*)CurrentThread);
}

void
thread_set_cthread_self(uint32_t cthr)
{
    thread_t curthr = current_thread();
    assert(curthr);
    curthr->machine.cthread_self = cthr;
    arm_set_threadpid_user_readonly((uint32_t*)curthr->machine.cthread_self);
}

uint64_t
thread_get_cthread_self(void)
{
    thread_t curthr = current_thread();
    assert(curthr);
    return curthr->machine.cthread_self;
}

/**
 * machine_thread_inherit_taskwide
 */
kern_return_t machine_thread_inherit_taskwide(thread_t thread, task_t parent_task)
{
	return KERN_FAILURE;
}

/**
 * machine_thread_create
 *
 * Do machine-dependent initialization for a thread.
 */
kern_return_t machine_thread_create(thread_t thread, task_t task)
{
    arm_saved_state_t*  sv = NULL;
    
    /* Create a thread and set the members in the pcb. */
    assert(thread != NULL);
    assert(thread->machine.iss == NULL);
    
    if (thread->machine.iss == NULL) {
        kmem_alloc_kobject(kernel_map, &sv, sizeof(arm_saved_state_t));
        if(!sv)
            panic("couldn't alloc savearea for thread\n");
    }

    /* Okay, got one. */
    bzero(sv, sizeof(arm_saved_state_t));

    /* Set the members now. */
    thread->machine.iss = sv;
    thread->machine.preempt_count = 0;
    thread->machine.cpu_data = cpu_datap(cpu_number());
    
    /* Also kernel threads */
    thread->machine.uss = thread->machine.iss;
            
    return KERN_SUCCESS;
}

/**
 * machine_switch_context
 *
 * Switch the current executing machine context to a new one.
 */ 
thread_t machine_switch_context(thread_t old, thread_continue_t continuation, thread_t new)
{
    pmap_t new_pmap;
    cpu_data_t* datap;
    register thread_t retval;

    kprintf("machine_switch_context: %p -> %p (cont: %p)\n", old, new, continuation);

#if 0
	if (old == new)
        panic("machine_switch_context: old = new thread (%p %p)", old, new);
#endif
    
    datap = cpu_datap(cpu_number());
    assert(datap != NULL);
    
    datap->old_thread = old;
    
    save_vfp_context(old);
    
    new_pmap = new->map->pmap;
    if ((old->map->pmap != new_pmap)) {
        if(new_pmap != NULL) {
             pmap_switch(new_pmap);
        }
    }
       
    /* VFP save needed */
        
	retval = Switch_context(old, continuation, new);
	assert(retval != NULL);

	return retval;
}


void
machine_stack_handoff(thread_t old,
                      thread_t new)
{
	vm_offset_t     stack;
    pmap_t          new_pmap;
    
	assert(new);
	assert(old);
    
	if (old == new)
		panic("machine_stack_handoff");
    
    kprintf("machine_stack_handoff: %p = old, %p = new\n", old, new);
    
	save_vfp_context(old);
    
	stack = machine_stack_detach(old);
	new->kernel_stack = stack;
    
    uint32_t *kstack = (uint32_t*)STACK_IKS(stack);
    new->machine.iss = (arm_saved_state_t*)kstack;
    new->machine.iss->sp = (uint32_t)kstack - sizeof(arm_saved_state_t);
    
    old->machine.iss = 0;
    
	if (stack == old->reserved_stack) {
		assert(new->reserved_stack);
		old->reserved_stack = new->reserved_stack;
		new->reserved_stack = stack;
	}
    
	/*
	 * A full call to machine_stack_attach() is unnecessry
	 * because old stack is already initialized.
	 */
    
    new_pmap = new->map->pmap;
    if ((old->map->pmap != new_pmap)) {
        if(new_pmap != NULL) {
            pmap_switch(new_pmap);
        }
    }
    
	machine_set_current_thread(new);
    
	return;
}

/**
 * machine_thread_init
 */
void machine_thread_init(void)
{
    return;
}

/** 
 * save_vfp_context
 *
 * Saves current vfp context into thread state.
 */
static void save_vfp_context(thread_t thread)
{
    assert(thread);
    if(thread->machine.vfp_enable && !thread->machine.vfp_dirty) {
        vfp_context_save(thread->machine.vfp_regs);
        vfp_enable_exception(FALSE);
    }
}

/**
 * machine_stack_attach
 *
 * Attach a stack to a thread and populate its members.
 */
void machine_stack_attach(thread_t thread, vm_offset_t stack)
{
    assert(stack != NULL);
    assert(thread != NULL);
    
    kprintf("machine_stack_attach: setting stack %p for thread %p\n",
            stack, thread);
    
    uint32_t *kstack = (uint32_t*)STACK_IKS(stack);
    
    thread->kernel_stack = stack;
    
    thread->machine.iss = (arm_saved_state_t*)kstack;
    
    thread->machine.iss->r[0] = (uint32_t)thread;
    thread->machine.iss->lr = (uint32_t)thread_continue;
    thread->machine.iss->sp = (uint32_t)kstack - sizeof(arm_saved_state_t);

    thread->machine.uss = thread->machine.iss;
    
    return;
}

/**
 * machine_stack_detach
 *
 * Kill the current stack.
 */
vm_offset_t machine_stack_detach(thread_t thread)
{
    vm_offset_t stack;
    
    assert(thread != NULL);
    
    kprintf("machine_stack_detach: killing stack for thread %p\n",
            thread);
    
    stack = thread->kernel_stack;

    return stack;
}

/*
 * Only one real processor.
 */
processor_t machine_choose_processor(processor_set_t pset, processor_t preferred)
{
    return (cpu_datap(cpu_number())->cpu_processor);
}

/**
 * call_continuation.
 *
 * Call the continuation routine for a thread, really is a shim for the
 * assembly routine.
 */
void call_continuation(thread_continue_t continuation, void *parameter, wait_result_t wresult)
{
    thread_t self = current_thread();
    
    assert(self->machine.iss != NULL);
    assert(self->kernel_stack);
    
    kprintf("call_continuation: calling continuation on thread %p\n", self);
    
    Call_continuation(continuation, parameter, wresult);
    
    return;
}

/**
 * machine_thread_destroy
 *
 * Destroy the machine specific thread context block.
 */
void
machine_thread_destroy(thread_t thread)
{
    return;
}

/*
 * This is where registers that are not normally specified by the mach-o
 * file on an execve would be nullified, perhaps to avoid a covert channel.
 */
kern_return_t
machine_thread_state_initialize(thread_t thread)
{
    return KERN_SUCCESS;
}

/*
 * This is called when a task is terminated, and also on exec().
 * Clear machine-dependent state that is stored on the task.
 */
void
machine_task_terminate(task_t task)
{
	return;
}
