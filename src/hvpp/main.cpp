#include "lib/mm.h"
#include "lib/assert.h"
#include "lib/log.h"

#include "hvpp/hypervisor.h"

#include "custom_vmexit.h"

#include <cinttypes>

#include <ntddk.h>

//////////////////////////////////////////////////////////////////////////
// Function prototypes.
//////////////////////////////////////////////////////////////////////////

using TVmExitHandler = custom_vmexit_handler;
static_assert(std::is_base_of_v<hvpp::vmexit_handler, TVmExitHandler>);

#define HVPP_MEMORY_TAG 'ppvh'

NTSTATUS
GlobalInitialize(
  _Out_ PVOID* Memory,
  _Out_ SIZE_T* MemorySize
  );

VOID
GlobalDestroy(
  _In_ PVOID Memory,
  _In_ SIZE_T MemorySize
  );

NTSTATUS
HvppInitialize(
  _Out_ hvpp::hypervisor** Hypervisor,
  _Out_ hvpp::vmexit_handler** VmExitHandler
  );

VOID
HvppDestroy(
  _In_ hvpp::hypervisor* Hypervisor,
  _In_ hvpp::vmexit_handler* VmExitHandler
  );

//////////////////////////////////////////////////////////////////////////
// Variables.
//////////////////////////////////////////////////////////////////////////

EXTERN_C DRIVER_INITIALIZE DriverEntry;

static PVOID                  HvppMemory        = nullptr;
static SIZE_T                 HvppMemorySize    = 0;

static hvpp::hypervisor*      HvppHypervisor    = nullptr;
static hvpp::vmexit_handler*  HvppVmExitHandler = nullptr;

//////////////////////////////////////////////////////////////////////////
// Function implementations.
//////////////////////////////////////////////////////////////////////////

NTSTATUS
GlobalInitialize(
  _In_ PVOID* Memory,
  _In_ SIZE_T* MemorySize
  )
{
  //
  // Initialize logger and memory manager.
  //
  logger::initialize();
  memory_manager::initialize();

  //
  // Print physical memory descriptor to the debugger.
  //
  memory_manager::physical_memory_descriptor().dump();

  //
  // Estimate required memory size. Make sure there's enough space for:
  //  - hypervisor instance
  //  - VCPU instance (for each logical processor)
  //  - 4kb EPT entries for whole physical memory (for each logical processor)
  //
  // To cover whole physical memory with 4kb page-table EPT entries, we need
  // (PhysicalMemorySize / 512) bytes. This number doesn't take into account
  // PD, PDPT and PML4 entries. We use that + additional 1/3 of the number of
  // bytes needed for PT (page-table). It should be enough not only for EPT
  // but also for eventual hooks or other memory allocations.
  //
  // For those still wondering:
  //   (PhysicalMemorySize / 512) + 33% ==
  //   (PhysicalMemorySize / 512) * (1 + 1/3) ==
  //   (PhysicalMemorySize / 384)
  //
  // If hypervisor begins to run out of memory, RequiredMemorySize is the right
  // variable to adjust.
  //
  ULONG  ProcessorCount     = KeQueryActiveProcessorCountEx(0);
  SIZE_T PhysicalMemorySize = memory_manager::physical_memory_descriptor().total_physical_memory_size();
  SIZE_T RequiredMemorySize = sizeof(hypervisor)                            +
                              (ProcessorCount * sizeof(vcpu_t))             +
                              (ProcessorCount * (PhysicalMemorySize / 384));

  RequiredMemorySize = BYTES_TO_PAGES(RequiredMemorySize) * PAGE_SIZE;

  hvpp_info("ProcessorCount:      %u", ProcessorCount);
  hvpp_info("PhysicalMemorySize:  %8" PRIu64 " kb", PhysicalMemorySize / 1024);
  hvpp_info("RequiredMemorySize:  %8" PRIu64 " kb", RequiredMemorySize / 1024);

  //
  // Allocate memory.
  //
  PVOID AllocatedMemory = ExAllocatePoolWithTag(NonPagedPool,
                                                RequiredMemorySize,
                                                HVPP_MEMORY_TAG);

  if (!AllocatedMemory)
  {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  //
  // Assign allocated memory to the memory manager.
  //
  memory_manager::assign(AllocatedMemory, RequiredMemorySize);

  *Memory     = AllocatedMemory;
  *MemorySize = RequiredMemorySize;

  return STATUS_SUCCESS;
}

VOID
GlobalDestroy(
  _In_ PVOID Memory,
  _In_ SIZE_T MemorySize
  )
{
  UNREFERENCED_PARAMETER(MemorySize);

  memory_manager::destroy();
  logger::destroy();

  ExFreePoolWithTag(Memory, HVPP_MEMORY_TAG);
}

NTSTATUS
HvppInitialize(
  _Out_ hvpp::hypervisor** Hypervisor,
  _Out_ hvpp::vmexit_handler** VmExitHandler
  )
{
  hvpp::hypervisor* HypervisorInstance = nullptr;
  hvpp::vmexit_handler* VmExitHandlerInstance = nullptr;

  //
  // Create hypervisor instance.
  //
  HypervisorInstance = new hvpp::hypervisor();

  if (!HypervisorInstance)
  {
    HvppDestroy(HypervisorInstance, VmExitHandlerInstance);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  //
  // Initialize hypervisor.
  //
  HypervisorInstance->initialize();

  //
  // Check if CPU is cappable of virtualization.
  //
  if (!HypervisorInstance->check())
  {
    HvppDestroy(HypervisorInstance, VmExitHandlerInstance);
    return STATUS_HV_FEATURE_UNAVAILABLE;
  }

  //
  // Create VM-exit handler instance.
  //
  VmExitHandlerInstance = new TVmExitHandler();
  if (!VmExitHandlerInstance)
  {
    HvppDestroy(HypervisorInstance, VmExitHandlerInstance);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  VmExitHandlerInstance->initialize();

  //
  // Propagate output values.
  //
  *Hypervisor = HypervisorInstance;
  *VmExitHandler = VmExitHandlerInstance;

  return STATUS_SUCCESS;
}

VOID
HvppDestroy(
  _In_ hvpp::hypervisor* Hypervisor,
  _In_ hvpp::vmexit_handler* VmExitHandler
  )
{
  if (Hypervisor)
  {
    Hypervisor->destroy();
    delete Hypervisor;
  }

  if (VmExitHandler)
  {
    VmExitHandler->destroy();
    delete VmExitHandler;
  }
}

EXTERN_C
VOID
DriverUnload(
  _In_ PDRIVER_OBJECT DriverObject
  )
{
  UNREFERENCED_PARAMETER(DriverObject);

  //
  // Stop hypervisor.
  //
  HvppHypervisor->stop();

  //
  // Free all memory.
  //
  HvppDestroy(HvppHypervisor, HvppVmExitHandler);

  //
  // Destroy memory manager and logger.
  //
  GlobalDestroy(HvppMemory, HvppMemorySize);
}

EXTERN_C
NTSTATUS
DriverEntry(
  _In_ PDRIVER_OBJECT DriverObject,
  _In_ PUNICODE_STRING RegistryPath
  )
{
  NTSTATUS Status;

  UNREFERENCED_PARAMETER(RegistryPath);

  DriverObject->DriverUnload = &DriverUnload;

  //
  // Initialize memory manager and logger.
  //
  Status = GlobalInitialize(&HvppMemory, &HvppMemorySize);
  if (!NT_SUCCESS(Status))
  {
    goto Exit;
  }

  //
  // Initialize hypervisor.
  //
  Status = HvppInitialize(&HvppHypervisor, &HvppVmExitHandler);
  if (!NT_SUCCESS(Status))
  {
    goto Exit;
  }

  //
  // Start the hypervisor.
  //
  HvppHypervisor->start(HvppVmExitHandler);

Exit:
  return Status;
}
