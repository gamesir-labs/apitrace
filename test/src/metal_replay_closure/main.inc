int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: test_metal_replay_closure <bundle> [bundle-check]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  const std::filesystem::path bundle_check = argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    std::cerr << "failed to open bundle\n";
    return 1;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_replay_closure";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {5, apitrace::trace::ObjectKind::Resource, 0, "drawable-texture"},
  });

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      100,
      "smoke.metallib",
      "fake-metallib-bytes");
  if (library.relative_path.empty()) {
    std::cerr << "failed to write Metal library asset\n";
    return 1;
  }
  const auto duplicate_library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      103,
      "smoke-duplicate.metallib",
      "fake-metallib-bytes");
  if (duplicate_library.blob_id == library.blob_id ||
      duplicate_library.relative_path != library.relative_path) {
    std::cerr << "duplicate Metal library did not share storage while preserving blob identity\n";
    return 1;
  }

  const std::string pipeline_descriptor =
      "{\"library_id\":1,\"vertex_library_id\":1,\"fragment_library_id\":1,"
      "\"vertex_function\":\"vs_main\",\"fragment_function\":\"fs_main\","
      "\"colors\":[{\"pixel_format\":\"bgra8unorm\",\"blending_enabled\":false,"
      "\"write_mask\":15,\"rgb_blend_operation\":0,\"alpha_blend_operation\":0,"
      "\"src_rgb_blend_factor\":1,\"dst_rgb_blend_factor\":0,"
      "\"src_alpha_blend_factor\":1,\"dst_alpha_blend_factor\":0}],"
      "\"rasterization_enabled\":true,"
      "\"raster_sample_count\":1}";
  const std::string compute_pipeline_descriptor =
      "{\"library_id\":1,\"function\":\"cs_main\"}";
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      101,
      "smoke.pipeline",
      pipeline_descriptor);
  if (pipeline.relative_path.empty()) {
    std::cerr << "failed to write Metal pipeline asset\n";
    return 1;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      10,
      6,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + duplicate_library.relative_path.generic_string() + "\",\"size\":18}",
      {duplicate_library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      11,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      12,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      13,
      7,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_depth_stencil_state\",\"depth_stencil_state_id\":7,"
      "\"depth_compare_function\":0,\"depth_write_enabled\":false}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      14,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      15,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      16,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 17, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      19,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_depth_stencil_state\",\"depth_stencil_state_id\":7,\"stencil_ref\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      20,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      21,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      22,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      23,
      3,
      "MTLCommandBuffer.commit",
      "{}"));

  apitrace::trace::AssetRecord present_frame;
  present_frame.blob_id = 102;
  present_frame.kind = apitrace::trace::AssetKind::Texture;
  present_frame.debug_name = "poisoned-diagnostic-metal-present-frame";
  present_frame.payload_bytes.assign(2 * 2 * 4, 0x00);
  present_frame = writer.register_asset(std::move(present_frame));
  apitrace::trace::EventRecord present_frame_event;
  present_frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
  present_frame_event.callsite.sequence = 1;
  present_frame_event.callsite.function_name = "resource_blob";
  present_frame_event.object_kind = apitrace::trace::ObjectKind::Unknown;
  present_frame_event.object_debug_name = "MetalPresentFrame";
  present_frame_event.blob_refs = {present_frame.blob_id};
  present_frame_event.payload =
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
      present_frame.relative_path.generic_string() + "\"}";
  writer.append_call_event(present_frame_event);
  writer.close();

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "failed to read bundle: " << reader.last_error() << "\n";
    return 1;
  }
  if (reader.metal_events().size() != 15) {
    std::cerr << "unexpected Metal event count\n";
    return 1;
  }
  if (reader.events().size() != 1 ||
      reader.events().front().object_debug_name != "MetalPresentFrame" ||
      reader.events().front().blob_refs.size() != 1 ||
      reader.events().front().blob_refs.front() != present_frame.blob_id) {
    std::cerr << "poisoned MetalPresentFrame diagnostic event was not preserved\n";
    return 1;
  }
  if (reader.events().front().payload.find(present_frame.relative_path.generic_string()) == std::string::npos) {
    std::cerr << "poisoned MetalPresentFrame diagnostic path was not preserved\n";
    return 1;
  }

  bool found_library_blob = false;
  bool found_duplicate_library_blob = false;
  bool found_pipeline_blob = false;
  for (const auto &asset : reader.metal_assets()) {
    found_library_blob = found_library_blob || asset.blob_id == library.blob_id;
    found_duplicate_library_blob = found_duplicate_library_blob || asset.blob_id == duplicate_library.blob_id;
    found_pipeline_blob = found_pipeline_blob || asset.blob_id == pipeline.blob_id;
  }
  if (!found_library_blob || !found_duplicate_library_blob || !found_pipeline_blob) {
    std::cerr << "Metal asset blob refs were not indexed by the reader\n";
    return 1;
  }

  std::string replay_error;
  apitrace::replay::ReplayStatistics replay_statistics;
  if (!validate_metal_bundle(bundle, replay_statistics, replay_error)) {
    std::cerr << "valid Metal bundle failed validate-only replay: " << replay_error << "\n";
    return 1;
  }
  if (replay_statistics.backend_name != "metal-validate-only" ||
      replay_statistics.metal_calls_replayed != reader.metal_events().size() ||
      replay_statistics.calls_replayed != 0 ||
      replay_statistics.presents_seen != 1 ||
      replay_statistics.metal_presents_seen != 1) {
    std::cerr << "poisoned MetalPresentFrame should remain diagnostic metadata, not replay input\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, bundle, "--require-metal --require-metal-replay-closure --require-metal-present-frames") != 0) {
    std::cerr << "bundle-check rejected the valid Metal replay closure fixture\n";
    return 1;
  }

  const auto sampler_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-sampler-descriptor");
  if (!write_sampler_descriptor_bundle(sampler_descriptor_bundle, "fake-metallib-bytes", pipeline_descriptor, true)) {
    std::cerr << "failed to write sampler-descriptor bundle\n";
    return 1;
  }
  if (!validate_metal_bundle(sampler_descriptor_bundle, replay_error)) {
    std::cerr << "sampler-descriptor bundle failed validate-only replay: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, sampler_descriptor_bundle, "--require-metal --require-metal-replay-closure") != 0) {
    std::cerr << "bundle-check rejected sampler-descriptor bundle\n";
    return 1;
  }

  const auto nullable_resource_bind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-nullable-resource-binds");
  if (!write_nullable_resource_bind_bundle(nullable_resource_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write nullable-resource-binds bundle\n";
    return 1;
  }
  if (!validate_metal_bundle(nullable_resource_bind_bundle, replay_error)) {
    std::cerr << "nullable-resource-binds bundle failed validate-only replay: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, nullable_resource_bind_bundle, "--require-metal --require-metal-replay-closure") != 0) {
    std::cerr << "bundle-check rejected nullable-resource-binds bundle\n";
    return 1;
  }

  const auto incomplete_sampler_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-incomplete-sampler-descriptor");
  if (!write_sampler_descriptor_bundle(
          incomplete_sampler_descriptor_bundle, "fake-metallib-bytes", pipeline_descriptor, false)) {
    std::cerr << "failed to write incomplete-sampler-descriptor bundle\n";
    return 1;
  }
  if (validate_metal_bundle(incomplete_sampler_descriptor_bundle, replay_error) ||
      !expect_contains(replay_error, "sampler descriptor is missing support_argument_buffers")) {
    std::cerr << "incomplete-sampler-descriptor bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, incomplete_sampler_descriptor_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted incomplete-sampler-descriptor bundle\n";
    return 1;
  }

  const auto missing_bind_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-bind");
  if (!write_missing_pipeline_bind_bundle(missing_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-pipeline-bind bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader missing_bind_reader;
  if (!missing_bind_reader.open(missing_bind_bundle)) {
    std::cerr << "failed to read missing-pipeline-bind bundle: " << missing_bind_reader.last_error() << "\n";
    return 1;
  }
  if (validate_metal_bundle(missing_bind_bundle, replay_error) ||
      !expect_contains(replay_error, "draw occurs before a valid render pipeline bind")) {
    std::cerr << "missing-pipeline-bind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_bind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-pipeline-bind bundle\n";
    return 1;
  }

  const auto invalid_viewport_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-invalid-viewport");
  if (!write_invalid_viewport_bundle(invalid_viewport_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write invalid-viewport bundle\n";
    return 1;
  }
  if (validate_metal_bundle(invalid_viewport_bundle, replay_error) ||
      !expect_contains(replay_error, "setViewport has invalid viewports")) {
    std::cerr << "invalid-viewport bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, invalid_viewport_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted invalid-viewport bundle\n";
    return 1;
  }

  const auto missing_color_texture_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-color-texture");
  if (!write_missing_color_texture_bundle(missing_color_texture_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-color-texture bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_color_texture_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass references an unknown color texture")) {
    std::cerr << "missing-color-texture bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_color_texture_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-color-texture bundle\n";
    return 1;
  }

  const auto render_pass_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-missing-payload");
  if (!write_render_pass_missing_payload_bundle(
          render_pass_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment is missing store_action")) {
    std::cerr << "render-pass-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-missing-payload bundle\n";
    return 1;
  }

  const auto render_pass_missing_subresource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-missing-subresource");
  if (!write_render_pass_missing_subresource_bundle(
          render_pass_missing_subresource_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-missing-subresource bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_missing_subresource_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment is missing level")) {
    std::cerr << "render-pass-missing-subresource bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_missing_subresource_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-missing-subresource bundle\n";
    return 1;
  }

  const auto render_pass_invalid_clear_color_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-invalid-clear-color");
  if (!write_render_pass_invalid_clear_color_bundle(
          render_pass_invalid_clear_color_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-invalid-clear-color bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_invalid_clear_color_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment has invalid clear_color")) {
    std::cerr << "render-pass-invalid-clear-color bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_invalid_clear_color_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-invalid-clear-color bundle\n";
    return 1;
  }

  const auto missing_blob_ref_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-blob-ref");
  if (!write_missing_pipeline_blob_ref_bundle(missing_blob_ref_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-blob-ref bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_blob_ref_bundle, replay_error) ||
      !expect_contains(replay_error, "asset path references are missing blob_refs")) {
    std::cerr << "missing-blob-ref bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_blob_ref_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-blob-ref bundle\n";
    return 1;
  }

  const auto wrong_blob_ref_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-blob-ref");
  if (!write_wrong_pipeline_blob_ref_bundle(wrong_blob_ref_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-blob-ref bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_blob_ref_bundle, replay_error) ||
      !expect_contains(replay_error, "asset path blob_ref does not match")) {
    std::cerr << "wrong-blob-ref bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_blob_ref_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-blob-ref bundle\n";
    return 1;
  }

  const auto incomplete_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-incomplete-descriptor");
  if (!write_incomplete_pipeline_descriptor_bundle(incomplete_descriptor_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write incomplete-descriptor bundle\n";
    return 1;
  }
  if (validate_metal_bundle(incomplete_descriptor_bundle, replay_error) ||
      !expect_contains(replay_error, "render pipeline descriptor missing vertex_function")) {
    std::cerr << "incomplete-descriptor bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, incomplete_descriptor_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted incomplete-descriptor bundle\n";
    return 1;
  }

  const auto pipeline_color_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-pipeline-color-missing-payload");
  if (!write_pipeline_color_missing_payload_bundle(pipeline_color_missing_payload_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write pipeline-color-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(pipeline_color_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "render pipeline color attachment 0 is missing blending_enabled")) {
    std::cerr << "pipeline-color-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          pipeline_color_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted pipeline-color-missing-payload bundle\n";
    return 1;
  }

  const auto wrong_buffer_type_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-vertex-buffer-type");
  if (!write_wrong_vertex_buffer_type_bundle(wrong_buffer_type_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-vertex-buffer-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_buffer_type_bundle, replay_error) ||
      !expect_contains(replay_error, "buffer bind references an unknown buffer")) {
    std::cerr << "wrong-vertex-buffer-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_buffer_type_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-vertex-buffer-type bundle\n";
    return 1;
  }

  const auto missing_offset_bind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-vertex-buffer-offset-bind");
  if (!write_missing_vertex_buffer_offset_bind_bundle(missing_offset_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-vertex-buffer-offset-bind bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_offset_bind_bundle, replay_error) ||
      !expect_contains(replay_error, "buffer offset update occurs before a matching vertex buffer bind")) {
    std::cerr << "missing-vertex-buffer-offset-bind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_offset_bind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-vertex-buffer-offset-bind bundle\n";
    return 1;
  }

  const auto missing_fragment_bytes_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-fragment-bytes");
  if (!write_missing_fragment_bytes_bundle(missing_fragment_bytes_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-fragment-bytes bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_fragment_bytes_bundle, replay_error) ||
      !expect_contains(replay_error, "setFragmentBytes is missing captured bytes")) {
    std::cerr << "missing-fragment-bytes bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_fragment_bytes_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-fragment-bytes bundle\n";
    return 1;
  }

  const auto texture_bind_missing_index_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-bind-missing-index");
  if (!write_texture_bind_missing_index_bundle(
          texture_bind_missing_index_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write texture-bind-missing-index bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_bind_missing_index_bundle, replay_error) ||
      !expect_contains(replay_error, "texture bind is missing index")) {
    std::cerr << "texture-bind-missing-index bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_bind_missing_index_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-bind-missing-index bundle\n";
    return 1;
  }

  const auto inline_bytes_missing_length_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-inline-bytes-missing-length");
  if (!write_inline_bytes_missing_length_bundle(
          inline_bytes_missing_length_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write inline-bytes-missing-length bundle\n";
    return 1;
  }
  if (validate_metal_bundle(inline_bytes_missing_length_bundle, replay_error) ||
      !expect_contains(replay_error, "setFragmentBytes is missing length")) {
    std::cerr << "inline-bytes-missing-length bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          inline_bytes_missing_length_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted inline-bytes-missing-length bundle\n";
    return 1;
  }

  const auto buffer_missing_length_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-missing-length");
  if (!write_buffer_missing_length_bundle(
          buffer_missing_length_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write buffer-missing-length bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_missing_length_bundle, replay_error) ||
      !expect_contains(replay_error, "MTLDevice.newBuffer is missing length")) {
    std::cerr << "buffer-missing-length bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_missing_length_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-missing-length bundle\n";
    return 1;
  }

  const auto open_encoder_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-open-encoder");
  if (!write_open_encoder_bundle(open_encoder_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write open-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(open_encoder_bundle, replay_error) ||
      !expect_contains(replay_error, "ended with an open render encoder")) {
    std::cerr << "open-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, open_encoder_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted open-encoder bundle\n";
    return 1;
  }

  const auto uncommitted_command_buffer_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-uncommitted-command-buffer");
  if (!write_uncommitted_command_buffer_bundle(uncommitted_command_buffer_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write uncommitted-command-buffer bundle\n";
    return 1;
  }
  if (validate_metal_bundle(uncommitted_command_buffer_bundle, replay_error) ||
      !expect_contains(replay_error, "ended with an uncommitted command buffer")) {
    std::cerr << "uncommitted-command-buffer bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, uncommitted_command_buffer_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted uncommitted-command-buffer bundle\n";
    return 1;
  }

  const auto argument_buffer_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-argument-buffer");
  if (!write_argument_buffer_bundle(argument_buffer_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write argument-buffer bundle\n";
    return 1;
  }
  if (validate_metal_bundle(argument_buffer_bundle, replay_error) ||
      !expect_contains(replay_error, "SetArgumentBuffer requires native Metal argument-buffer replay support")) {
    std::cerr << "argument-buffer bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, argument_buffer_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted argument-buffer bundle\n";
    return 1;
  }

  const auto wrong_use_resource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-use-resource-type");
  if (!write_wrong_use_resource_type_bundle(wrong_use_resource_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-use-resource-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_use_resource_bundle, replay_error) ||
      !expect_contains(replay_error, "useResource references an unknown replayable resource")) {
    std::cerr << "wrong-use-resource-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_use_resource_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-use-resource-type bundle\n";
    return 1;
  }

  const auto use_resource_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-use-resource-missing-payload");
  if (!write_use_resource_missing_payload_bundle(
          use_resource_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write use-resource-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(use_resource_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "useResource is missing stages")) {
    std::cerr << "use-resource-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          use_resource_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted use-resource-missing-payload bundle\n";
    return 1;
  }

  const auto use_heap_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-use-heap");
  if (!write_use_heap_bundle(use_heap_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write use-heap bundle\n";
    return 1;
  }
  if (validate_metal_bundle(use_heap_bundle, replay_error) ||
      !expect_contains(replay_error, "UseHeap requires native Metal heap replay support")) {
    std::cerr << "use-heap bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, use_heap_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted use-heap bundle\n";
    return 1;
  }

  const auto unknown_encoder_state_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-encoder-state");
  if (!write_unknown_encoder_state_bundle(unknown_encoder_state_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-encoder-state bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_encoder_state_bundle, replay_error) ||
      !expect_contains(replay_error, "encoder state command references an unknown render encoder")) {
    std::cerr << "unknown-encoder-state bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_encoder_state_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-encoder-state bundle\n";
    return 1;
  }

  const auto rasterizer_state_missing_field_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-rasterizer-state-missing-field");
  if (!write_rasterizer_state_missing_field_bundle(
          rasterizer_state_missing_field_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write rasterizer-state-missing-field bundle\n";
    return 1;
  }
  if (validate_metal_bundle(rasterizer_state_missing_field_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_set_rasterizer_state is missing winding")) {
    std::cerr << "rasterizer-state-missing-field bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, rasterizer_state_missing_field_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted rasterizer-state-missing-field bundle\n";
    return 1;
  }

  const auto unknown_call_kind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-call-kind");
  if (!write_unknown_call_kind_bundle(unknown_call_kind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-call-kind bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_call_kind_bundle, replay_error) ||
      !expect_contains(replay_error, "unknown Metal call kind is not replayable")) {
    std::cerr << "unknown-call-kind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_call_kind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-call-kind bundle\n";
    return 1;
  }

  const auto unknown_depth_stencil_state_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-depth-stencil-state");
  if (!write_unknown_depth_stencil_state_bundle(
          unknown_depth_stencil_state_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-depth-stencil-state bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_depth_stencil_state_bundle, replay_error) ||
      !expect_contains(replay_error, "encoder state references an unknown depth stencil state")) {
    std::cerr << "unknown-depth-stencil-state bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_depth_stencil_state_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-depth-stencil-state bundle\n";
    return 1;
  }

  const auto depth_stencil_state_missing_id_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-depth-stencil-state-missing-id");
  if (!write_depth_stencil_state_missing_id_bundle(
          depth_stencil_state_missing_id_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write depth-stencil-state-missing-id bundle\n";
    return 1;
  }
  if (validate_metal_bundle(depth_stencil_state_missing_id_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_set_depth_stencil_state is missing depth_stencil_state_id")) {
    std::cerr << "depth-stencil-state-missing-id bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          depth_stencil_state_missing_id_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted depth-stencil-state-missing-id bundle\n";
    return 1;
  }

  const auto texture_view_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-missing-payload");
  if (!write_texture_view_missing_payload_bundle(texture_view_missing_payload_bundle)) {
    std::cerr << "failed to write texture-view-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor is missing swizzle")) {
    std::cerr << "texture-view-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-missing-payload bundle\n";
    return 1;
  }

  const auto texture_view_invalid_swizzle_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-invalid-swizzle");
  if (!write_texture_view_invalid_swizzle_bundle(texture_view_invalid_swizzle_bundle)) {
    std::cerr << "failed to write texture-view-invalid-swizzle bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_invalid_swizzle_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor has invalid swizzle")) {
    std::cerr << "texture-view-invalid-swizzle bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_invalid_swizzle_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-invalid-swizzle bundle\n";
    return 1;
  }

  const auto texture_view_unknown_swizzle_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-unknown-swizzle");
  if (!write_texture_view_unknown_swizzle_bundle(texture_view_unknown_swizzle_bundle)) {
    std::cerr << "failed to write texture-view-unknown-swizzle bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_unknown_swizzle_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor has invalid swizzle")) {
    std::cerr << "texture-view-unknown-swizzle bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_unknown_swizzle_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-unknown-swizzle bundle\n";
    return 1;
  }

  const auto depth_stencil_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-depth-stencil-missing-payload");
  if (!write_depth_stencil_missing_payload_bundle(depth_stencil_missing_payload_bundle)) {
    std::cerr << "failed to write depth-stencil-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(depth_stencil_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "depth stencil descriptor is missing depth_write_enabled")) {
    std::cerr << "depth-stencil-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          depth_stencil_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted depth-stencil-missing-payload bundle\n";
    return 1;
  }

  const auto buffer_gpu_address_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-gpu-address-missing-payload");
  if (!write_buffer_gpu_address_missing_payload_bundle(buffer_gpu_address_missing_payload_bundle)) {
    std::cerr << "failed to write buffer-gpu-address-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_gpu_address_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_buffer_gpu_address is missing gpu_address")) {
    std::cerr << "buffer-gpu-address-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_gpu_address_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-gpu-address-missing-payload bundle\n";
    return 1;
  }

  const auto buffer_gpu_address_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-gpu-address-signpost-missing-payload");
  if (!write_buffer_gpu_address_signpost_missing_payload_bundle(buffer_gpu_address_signpost_missing_payload_bundle)) {
    std::cerr << "failed to write buffer-gpu-address-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_gpu_address_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_buffer_gpu_address is missing buffer_id")) {
    std::cerr << "buffer-gpu-address-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_gpu_address_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-gpu-address-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto texture_descriptor_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-descriptor-missing-payload");
  if (!write_texture_descriptor_missing_payload_bundle(texture_descriptor_missing_payload_bundle)) {
    std::cerr << "failed to write texture-descriptor-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_descriptor_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "texture descriptor is missing sample_count")) {
    std::cerr << "texture-descriptor-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_descriptor_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-descriptor-missing-payload bundle\n";
    return 1;
  }

  const auto texture_descriptor_invalid_pixel_format_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-descriptor-invalid-pixel-format");
  if (!write_texture_descriptor_invalid_pixel_format_bundle(texture_descriptor_invalid_pixel_format_bundle)) {
    std::cerr << "failed to write texture-descriptor-invalid-pixel-format bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_descriptor_invalid_pixel_format_bundle, replay_error) ||
      !expect_contains(replay_error, "texture descriptor has invalid pixel_format")) {
    std::cerr << "texture-descriptor-invalid-pixel-format bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_descriptor_invalid_pixel_format_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-descriptor-invalid-pixel-format bundle\n";
    return 1;
  }

  const auto texture_gpu_resource_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-gpu-resource-missing-payload");
  if (!write_texture_gpu_resource_missing_payload_bundle(texture_gpu_resource_missing_payload_bundle)) {
    std::cerr << "failed to write texture-gpu-resource-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_gpu_resource_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_texture_gpu_resource_id is missing gpu_resource_id")) {
    std::cerr << "texture-gpu-resource-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_gpu_resource_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-gpu-resource-missing-payload bundle\n";
    return 1;
  }

  const auto memory_barrier_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-memory-barrier-missing-payload");
  if (!write_memory_barrier_missing_payload_bundle(
          memory_barrier_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write memory-barrier-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(memory_barrier_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "memoryBarrier is missing stages_after")) {
    std::cerr << "memory-barrier-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          memory_barrier_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted memory-barrier-missing-payload bundle\n";
    return 1;
  }

  const auto fence_ops_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-fence-ops-missing-payload");
  if (!write_fence_ops_missing_payload_bundle(
          fence_ops_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write fence-ops-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(fence_ops_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "fenceOps is missing ops")) {
    std::cerr << "fence-ops-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          fence_ops_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted fence-ops-missing-payload bundle\n";
    return 1;
  }

  const auto dispatch_signpost_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-signpost-without-pipeline");
  if (!write_dispatch_signpost_without_pipeline_bundle(dispatch_signpost_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write dispatch-signpost-without-pipeline bundle\n";
    return 1;
  }
  if (validate_metal_bundle(dispatch_signpost_bundle, replay_error) ||
      !expect_contains(replay_error, "dispatch signpost occurs before a valid compute pipeline bind")) {
    std::cerr << "dispatch-signpost-without-pipeline bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, dispatch_signpost_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted dispatch-signpost-without-pipeline bundle\n";
    return 1;
  }

  const auto dispatch_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-signpost-missing-payload");
  if (!write_dispatch_signpost_missing_payload_bundle(
          dispatch_signpost_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write dispatch-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(dispatch_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dispatchThreads is missing height")) {
    std::cerr << "dispatch-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          dispatch_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted dispatch-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto compute_bytes_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-without-encoder");
  if (!write_compute_bytes_without_encoder_bundle(compute_bytes_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-without-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_bundle, replay_error) ||
      !expect_contains(replay_error, "setComputeBytes references an unknown compute encoder")) {
    std::cerr << "compute-bytes-without-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, compute_bytes_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-without-encoder bundle\n";
    return 1;
  }

  const auto compute_bytes_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-missing-payload");
  if (!write_compute_bytes_missing_payload_bundle(
          compute_bytes_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "setComputeBytes is missing captured bytes")) {
    std::cerr << "compute-bytes-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, compute_bytes_missing_payload_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-missing-payload bundle\n";
    return 1;
  }

  const auto compute_bytes_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-signpost-missing-payload");
  if (!write_compute_bytes_signpost_missing_payload_bundle(
          compute_bytes_signpost_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "compute bytes signpost is missing captured bytes")) {
    std::cerr << "compute-bytes-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          compute_bytes_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto copy_signpost_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-signpost-without-blit-encoder");
  if (!write_copy_signpost_without_blit_encoder_bundle(
          copy_signpost_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write copy-signpost-without-blit-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_signpost_bundle, replay_error) ||
      !expect_contains(replay_error, "copy buffer to texture signpost references an unknown blit encoder")) {
    std::cerr << "copy-signpost-without-blit-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, copy_signpost_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-signpost-without-blit-encoder bundle\n";
    return 1;
  }

  const auto copy_buffer_missing_size_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-buffer-missing-size");
  if (!write_copy_buffer_missing_size_bundle(
          copy_buffer_missing_size_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write copy-buffer-missing-size bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_buffer_missing_size_bundle, replay_error) ||
      !expect_contains(replay_error, "copyFromBuffer is missing size")) {
    std::cerr << "copy-buffer-missing-size bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, copy_buffer_missing_size_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-buffer-missing-size bundle\n";
    return 1;
  }

  const auto copy_texture_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-missing-payload");
  if (!write_copy_texture_missing_payload_bundle(copy_texture_missing_payload_bundle)) {
    std::cerr << "failed to write copy-texture-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_texture_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "copyFromTexture is missing source_slice")) {
    std::cerr << "copy-texture-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          copy_texture_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-texture-missing-payload bundle\n";
    return 1;
  }

  const auto blit_fill_missing_value_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-fill-missing-value");
  if (!write_blit_fill_missing_value_bundle(
          blit_fill_missing_value_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-fill-missing-value bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_fill_missing_value_bundle, replay_error) ||
      !expect_contains(replay_error, "fillBuffer is missing value")) {
    std::cerr << "blit-fill-missing-value bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          blit_fill_missing_value_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-fill-missing-value bundle\n";
    return 1;
  }

  const auto blit_batch_missing_ops_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-batch-missing-ops");
  if (!write_blit_batch_missing_ops_bundle(
          blit_batch_missing_ops_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-batch-missing-ops bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_batch_missing_ops_bundle, replay_error) ||
      !expect_contains(replay_error, "blitBatch is missing ops")) {
    std::cerr << "blit-batch-missing-ops bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, blit_batch_missing_ops_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-batch-missing-ops bundle\n";
    return 1;
  }

  const auto blit_batch_fence_missing_stages_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-batch-fence-missing-stages");
  if (!write_blit_batch_fence_missing_stages_bundle(
          blit_batch_fence_missing_stages_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-batch-fence-missing-stages bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_batch_fence_missing_stages_bundle, replay_error) ||
      !expect_contains(replay_error, "wait_fence blitBatch op is missing stages")) {
    std::cerr << "blit-batch-fence-missing-stages bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          blit_batch_fence_missing_stages_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-batch-fence-missing-stages bundle\n";
    return 1;
  }

  const auto draw_missing_vertex_count_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-missing-vertex-count");
  if (!write_draw_missing_vertex_count_bundle(
          draw_missing_vertex_count_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write draw-missing-vertex-count bundle\n";
    return 1;
  }
  if (validate_metal_bundle(draw_missing_vertex_count_bundle, replay_error) ||
      !expect_contains(replay_error, "drawPrimitives is missing vertex_count")) {
    std::cerr << "draw-missing-vertex-count bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, draw_missing_vertex_count_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted draw-missing-vertex-count bundle\n";
    return 1;
  }

  const auto draw_invalid_primitive_type_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-invalid-primitive-type");
  if (!write_draw_invalid_primitive_type_bundle(
          draw_invalid_primitive_type_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write draw-invalid-primitive-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(draw_invalid_primitive_type_bundle, replay_error) ||
      !expect_contains(replay_error, "drawPrimitives has invalid primitive_type")) {
    std::cerr << "draw-invalid-primitive-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, draw_invalid_primitive_type_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted draw-invalid-primitive-type bundle\n";
    return 1;
  }

  const auto present_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-present-missing-payload");
  if (!write_present_missing_payload_bundle(
          present_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write present-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(present_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "presentDrawable is missing width")) {
    std::cerr << "present-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          present_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted present-missing-payload bundle\n";
    return 1;
  }

  return 0;
}
