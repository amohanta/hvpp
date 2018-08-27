#pragma once
#include "ept.h"

#include "ia32/arch.h"
#include "ia32/exception.h"
#include "ia32/vmx.h"

#include <cstdint>

namespace hvpp {

using namespace ia32;

class vmexit_handler;

static constexpr int vcpu_stack_size = 0x8000;

struct interrupt_info_t
{
  public:
    interrupt_info_t(vmx::interrupt_type intr_type, exception_vector expt_vector,
                     int rip_adjust = -1) noexcept
      : interrupt_info_t(intr_type, expt_vector, exception_error_code_t{ 0 }, false, rip_adjust) { }

    interrupt_info_t(vmx::interrupt_type intr_type, exception_vector expt_vector,
                     exception_error_code_t expt_code, int rip_adjust = -1) noexcept
      : interrupt_info_t(intr_type, expt_vector, expt_code, true, rip_adjust) { }

    interrupt_info_t(const interrupt_info_t& other) noexcept = default;
    interrupt_info_t(interrupt_info_t&& other) noexcept = default;
    interrupt_info_t& operator=(const interrupt_info_t& other) noexcept = default;
    interrupt_info_t& operator=(interrupt_info_t&& other) noexcept = default;

    auto vector() const noexcept { return static_cast<exception_vector>(info_.vector); }
    auto type() const noexcept { return static_cast<vmx::interrupt_type>(info_.type); }
    auto error_code() const noexcept { return error_code_; }
    int  rip_adjust() const noexcept { return rip_adjust_; }

    bool error_code_valid() const noexcept { return info_.error_code_valid; }
    bool nmi_unblocking() const noexcept { return info_.nmi_unblocking; }
    bool valid() const noexcept { return info_.valid; }

  private:
    friend class vcpu_t;

    interrupt_info_t() noexcept : info_(), error_code_(), rip_adjust_() { }
    interrupt_info_t(vmx::interrupt_type interrupt_type, exception_vector exception_vector,
                     exception_error_code_t exception_code, bool exception_code_valid,
                     int rip_adjust) noexcept
    {
      info_.flags = 0;

      info_.vector = static_cast<uint32_t>(exception_vector);
      info_.type   = static_cast<uint32_t>(interrupt_type);
      info_.valid  = true;

      //
      // Final sanitization of the following fields takes place in vcpu::inject().
      //

      info_.error_code_valid = exception_code_valid;
      error_code_  = exception_code;
      rip_adjust_  = rip_adjust;
    }

    vmx::interrupt_info_t info_;
    exception_error_code_t error_code_;
    int rip_adjust_;
};

enum class vcpu_state
{
  //
  // VCPU is unitialized.
  //
  off,

  //
  // VCPU is in VMX root mode; host & guest VMCS is being initialized.
  //
  initializing,

  //
  // VCPU successfully performed its initial VMENTRY.
  //
  launching,

  //
  // VCPU is running.
  //
  running,

  //
  // VCPU is terminating; vcpu::destroy has been called.
  //
  terminating,

  //
  // VCPU is terminated, VMX root mode has been left.
  //
  terminated,
};

class vcpu_t
{
  public:
    void initialize(vmexit_handler* handler = nullptr) noexcept;
    void destroy() noexcept;

    void launch() noexcept;
    void terminate() noexcept;

    auto exit_handler() const noexcept -> vmexit_handler*;
    void exit_handler(vmexit_handler* handler) noexcept;

    ept_t& ept() noexcept { return ept_; }

    context_t& exit_context() { return exit_context_; }
    void suppress_rip_adjust() noexcept { suppress_rip_adjust_ = true; }

    //
    // VMCS manipulation. Implementation is in vcpu.inl.
    //

  public:
    auto exit_interrupt_info() const noexcept -> interrupt_info_t;
    void inject(interrupt_info_t interrupt) noexcept;

    auto exit_instruction_info_guest_va() const noexcept -> void*;

  private:
    //
    // Control state
    //

  public:
    auto vcpu_id() const noexcept -> uint16_t;

  private:
    void vcpu_id(uint16_t virtual_processor_identifier) noexcept;
    auto ept_pointer() const noexcept -> ept_ptr_t;
    void ept_pointer(ept_ptr_t ept_pointer) noexcept;
    auto vmcs_link_pointer() const noexcept -> pa_t;     // technically, this is guest state
    void vmcs_link_pointer(pa_t link_pointer) noexcept;

  public:
    auto pin_based_controls() const noexcept -> msr::vmx_pinbased_ctls_t;
    void pin_based_controls(msr::vmx_pinbased_ctls_t controls) noexcept;

    auto processor_based_controls() const noexcept -> msr::vmx_procbased_ctls_t;
    void processor_based_controls(msr::vmx_procbased_ctls_t controls) noexcept;

    auto processor_based_controls2() const noexcept -> msr::vmx_procbased_ctls2_t;
    void processor_based_controls2(msr::vmx_procbased_ctls2_t controls) noexcept;

    auto vm_entry_controls() const noexcept -> msr::vmx_entry_ctls_t;
    void vm_entry_controls(msr::vmx_entry_ctls_t controls) noexcept;

    auto vm_exit_controls() const noexcept -> msr::vmx_exit_ctls_t;
    void vm_exit_controls(msr::vmx_exit_ctls_t controls) noexcept;

    auto exception_bitmap() const noexcept -> vmx::exception_bitmap_t;
    void exception_bitmap(vmx::exception_bitmap_t exception_bitmap) noexcept;
    auto msr_bitmap() const noexcept -> const vmx::msr_bitmap_t&;
    void msr_bitmap(const vmx::msr_bitmap_t& msr_bitmap) noexcept;
    auto io_bitmap() const noexcept -> const vmx::io_bitmap_t&;
    void io_bitmap(const vmx::io_bitmap_t& io_bitmap) noexcept;

    auto pagefault_error_code_mask() const noexcept -> pagefault_error_code_t;
    void pagefault_error_code_mask(pagefault_error_code_t mask) noexcept;
    auto pagefault_error_code_match() const noexcept -> pagefault_error_code_t;
    void pagefault_error_code_match(pagefault_error_code_t match) noexcept;

    //
    // Control entry state
    //

    auto cr0_guest_host_mask() const noexcept -> cr0_t;
    void cr0_guest_host_mask(cr0_t cr0) noexcept;
    auto cr0_shadow() const noexcept -> cr0_t;
    void cr0_shadow(cr0_t cr0) noexcept;
    auto cr4_guest_host_mask() const noexcept -> cr4_t;
    void cr4_guest_host_mask(cr4_t cr4) noexcept;
    auto cr4_shadow() const noexcept -> cr4_t;
    void cr4_shadow(cr4_t cr4) noexcept;

    auto entry_instruction_length() const noexcept -> uint32_t;
    void entry_instruction_length(uint32_t instruction_length) noexcept;

    auto entry_interruption_info() const noexcept -> vmx::interrupt_info_t;
    void entry_interruption_info(vmx::interrupt_info_t info) noexcept;

    auto entry_interruption_error_code() const noexcept -> exception_error_code_t;
    void entry_interruption_error_code(exception_error_code_t error_code) noexcept;

    //
    // Exit state
    //

    auto exit_instruction_error() const noexcept -> vmx::instruction_error;
    auto exit_instruction_info() const noexcept -> vmx::instruction_info_t;
    auto exit_instruction_length() const noexcept -> uint32_t;

    auto exit_interruption_info() const noexcept -> vmx::interrupt_info_t;
    auto exit_interruption_error_code() const noexcept -> exception_error_code_t;

    auto exit_reason() const noexcept -> vmx::exit_reason;
    auto exit_qualification() const noexcept -> vmx::exit_qualification_t;
    auto exit_guest_physical_address() const noexcept -> pa_t;
    auto exit_guest_linear_address() const noexcept -> la_t;

    //
    // Guest state
    //

    auto guest_cr0() const noexcept -> cr0_t;
    void guest_cr0(cr0_t cr0) noexcept;
    auto guest_cr3() const noexcept -> cr3_t;
    void guest_cr3(cr3_t cr3) noexcept;
    auto guest_cr4() const noexcept -> cr4_t;
    void guest_cr4(cr4_t cr4) noexcept;

    auto guest_dr7() const noexcept -> dr7_t;
    void guest_dr7(dr7_t dr7) noexcept;
    auto guest_debugctl() const noexcept -> msr::debugctl_t;
    void guest_debugctl(msr::debugctl_t debugctl) noexcept;

    auto guest_rsp() const noexcept -> uint64_t;
    void guest_rsp(uint64_t rsp) noexcept;
    auto guest_rip() const noexcept -> uint64_t;
    void guest_rip(uint64_t rip) noexcept;
    auto guest_rflags() const noexcept -> rflags_t;
    void guest_rflags(rflags_t rflags) noexcept;

    auto guest_gdtr() const noexcept -> gdtr_t;
    void guest_gdtr(gdtr_t gdtr) noexcept;
    auto guest_idtr() const noexcept -> idtr_t;
    void guest_idtr(idtr_t idtr) noexcept;

    auto guest_cs() const noexcept -> seg_t<cs_t>;
    void guest_cs(seg_t<cs_t> cs) noexcept;
    auto guest_ds() const noexcept -> seg_t<ds_t>;
    void guest_ds(seg_t<ds_t> ds) noexcept;
    auto guest_es() const noexcept -> seg_t<es_t>;
    void guest_es(seg_t<es_t> es) noexcept;
    auto guest_fs() const noexcept -> seg_t<fs_t>;
    void guest_fs(seg_t<fs_t> fs) noexcept;
    auto guest_gs() const noexcept -> seg_t<gs_t>;
    void guest_gs(seg_t<gs_t> gs) noexcept;
    auto guest_ss() const noexcept -> seg_t<ss_t>;
    void guest_ss(seg_t<ss_t> ss) noexcept;
    auto guest_tr() const noexcept -> seg_t<tr_t>;
    void guest_tr(seg_t<tr_t> tr) noexcept;
    auto guest_ldtr() const noexcept -> seg_t<ldtr_t>;
    void guest_ldtr(seg_t<ldtr_t> ldtr) noexcept;

    auto guest_segment_base_address(int index) const noexcept -> void*;
    void guest_segment_base_address(int index, void* base_address) noexcept;
    auto guest_segment_limit(int index) const noexcept -> uint32_t;
    void guest_segment_limit(int index, uint32_t limit) noexcept;
    auto guest_segment_access(int index) const noexcept -> seg_access_vmx_t;
    void guest_segment_access(int index, seg_access_vmx_t access_rights) noexcept;
    auto guest_segment_selector(int index) const noexcept -> seg_selector_t;
    void guest_segment_selector(int index, seg_selector_t selector) noexcept;

    auto guest_segment(int index) const noexcept -> seg_t<>;
    void guest_segment(int index, seg_t<> seg) noexcept;

  private:
    //
    // Host state
    //

    auto host_cr0() const noexcept -> cr0_t;
    void host_cr0(cr0_t cr0) noexcept;
    auto host_cr3() const noexcept -> cr3_t;
    void host_cr3(cr3_t cr3) noexcept;
    auto host_cr4() const noexcept -> cr4_t;
    void host_cr4(cr4_t cr4) noexcept;

    auto host_rsp() const noexcept -> uint64_t;
    void host_rsp(uint64_t rsp) noexcept;
    auto host_rip() const noexcept -> uint64_t;
    void host_rip(uint64_t rip) noexcept;

    auto host_gdtr() const noexcept -> gdtr_t;
    void host_gdtr(gdtr_t gdtr) noexcept;
    auto host_idtr() const noexcept -> idtr_t;
    void host_idtr(idtr_t idtr) noexcept;

    auto host_cs() const noexcept -> seg_t<cs_t>;
    void host_cs(seg_t<cs_t> cs) noexcept;
    auto host_ds() const noexcept -> seg_t<ds_t>;
    void host_ds(seg_t<ds_t> ds) noexcept;
    auto host_es() const noexcept -> seg_t<es_t>;
    void host_es(seg_t<es_t> es) noexcept;
    auto host_fs() const noexcept -> seg_t<fs_t>;
    void host_fs(seg_t<fs_t> fs) noexcept;
    auto host_gs() const noexcept -> seg_t<gs_t>;
    void host_gs(seg_t<gs_t> gs) noexcept;
    auto host_ss() const noexcept -> seg_t<ss_t>;
    void host_ss(seg_t<ss_t> ss) noexcept;
    auto host_tr() const noexcept -> seg_t<tr_t>;
    void host_tr(seg_t<tr_t> tr) noexcept;

    //
    // ldtr does not exist in VMX root mode.
    //
    // auto host_ldtr() const noexcept -> seg_t<ldtr_t>;
    // void host_ldtr(seg_t<ldtr_t> ldtr)  noexcept;
    //

  private:
    void error() noexcept;
    void setup() noexcept;

    void load_vmxon() noexcept;
    void load_vmcs() noexcept;

    void setup_host() noexcept;
    void setup_guest() noexcept;

    void entry_host() noexcept;
    void entry_guest() noexcept;

    static void entry_host_() noexcept;
    static void entry_guest_() noexcept;

    //
    // If you reorder following three members (stack, guest context and exit
    // context), you have to edit offsets in vcpu.asm.
    //
    uint8_t            stack_[vcpu_stack_size];
    context_t          guest_context_;
    context_t          exit_context_;

    //
    // Various VMX structures. Keep in mind they have "alignas(PAGE_SIZE)"
    // specifier.
    //
    vmx::vmcs_t        vmxon_;
    vmx::vmcs_t        vmcs_;
    vmx::msr_bitmap_t  msr_bitmap_;
    vmx::io_bitmap_t   io_bitmap_;

    //
    // FXSAVE area - to keep SSE registers sane between VM-exits.
    //
    fxsave_area_t      fxsave_area_;

    vmexit_handler*    handler_;
    vcpu_state         state_;
    ept_t              ept_;
    bool               suppress_rip_adjust_;
};

}
