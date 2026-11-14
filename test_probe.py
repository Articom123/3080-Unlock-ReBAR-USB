"""Host mocks for GCD reservation/cleanup and address validation, never efi_main."""
import ctypes
import sys

dll = ctypes.CDLL(sys.argv[1])
tests = (
    'reserve_verified_release', 'refusal_requires_unchanged_map',
    'foreign_owner_is_not_freed', 'query_failure_still_releases',
    'release_failure_blocks_handoff', 'wrong_address_is_not_freed',
    'no_candidate_or_map_means_no_allocation',
    'region_type_owner_alignment_and_boundary', 'framebuffer_exclusion',
    'map_validation', 'range_coverage_rejects_gap_and_foreign_middle',
    'gpu_change_gate', 'image_capture_memory_bounds', 'acpi_capture_memory_bounds',
    'move_success_and_restoration_order', 'move_each_outbound_write_failure',
    'move_restore_failure_halts', 'move_readback_failures',
    'move_samples_and_preconditions', 'move_requires_verified_reservation',
    'move_preflight_rejects_changed_hardware', 'move_live_write_and_read_bounds',
    'resize_capability_and_size_failures', 'resize_probe_and_shrink_order',
    'resize_capability_chain_bounds',
    'handoff_pixels_stride_and_roundtrip', 'handoff_overlap_copies',
    'handoff_invalid_rectangles_and_buffers', 'handoff_mode_and_pixel_selftest',
    'handoff_pci_translation_and_bounds', 'handoff_bar_descriptors',
    'handoff_commit_or_rollback', 'handoff_retains_owned_destination',
    'handoff_changed_mode_and_readonly_storage',
    'handoff_discovers_compact_and_sparse_bars',
    'handoff_rejects_ambiguous_or_invalid_resources',
    'handoff_compact_access_and_io_delegation',
    'handoff_preserves_native_range_max_convention',
    'universal_discovers_intel_layout', 'universal_refuses_unsafe_layouts',
    'universal_chipset_slot_refused', 'universal_preflight_and_move_on_intel',
    'universal_region_choice_on_intel', 'universal_windows_guid_from_boot_entry',
    'universal_empty_descriptor_is_no_bar', 'universal_unlock_then_resize_hook', 'optional_logging_and_failures',
)
for name in tests:
    result = getattr(dll, 'test_' + name)()
    if result:
        raise AssertionError(f'{name}: C line {result}')
    print(f'{name}: passed')
print(f'{len(tests)} mock tests passed; physical firmware behavior remains untested.')
