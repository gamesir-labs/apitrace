// D3D12 replay-model serialization (compact sequential binary format).
//
// Persists the reconstructed D3D12ReplayBackend object model so retrace can skip the in-process
// initialize+replay_event reconstruction. Included into d3d12_replay.cpp after backend_methods.inc,
// so these are member-function definitions with full private access to the backend containers.
//
// Rationale: an earlier JSON-DOM layout proved fatally slow to LOAD on real bundles -- nlohmann's
// per-character lexer plus DOM materialization made parsing a multi-GB document slower (and far more
// memory-hungry) than the reconstruction it was meant to replace. The model is structurally large
// (millions of scalar records), so the structure itself -- not just the bulk blobs -- needs a dumb,
// sequential, memcpy-style container.
//
// Layout: two files, BOTH binary. The names are kept as-is (kD3D12ReplayModelJsonName /
// kD3D12ReplayModelBlobName) so finalize/retrace/bundle-check need no change; the ".json" name is
// now historical.
//   * json_path -- STRUCTURAL stream: 8-byte magic, u32 schema version, source-bundle hash, blob-pool
//     size, then every container written back-to-back.
//   * blob_path -- append-only byte pool for bulk byte arrays and large strings (root-signature
//     bytes, resource data-update bytes, replay-command payload/function_name, tile-mapping payload);
//     the structural stream references pool slices inline as a {u64 offset, u64 length} pair.
//
// Encoding (documented assumption: NATIVE little-endian only -- this is a local on-disk cache, never
// a cross-architecture transport; both mac-native and wine-PE targets are x86_64/arm64 LE):
//   scalars -> raw sizeof(T); std::string / std::filesystem::path -> u64 length + bytes (path via
//   generic_string()); fixed C arrays -> N raw elements; std::vector<T> -> u64 count + elements;
//   unordered_map<K,V> -> u64 count + {K,V} pairs SORTED by key on write (deterministic byte image).
//
// A single serialize_fields(Archive&, T&) per struct drives BOTH directions (the Archive is either a
// writer or a reader), structurally guaranteeing the written and read field sets cannot drift apart.
// All helpers live in an anonymous namespace local to this translation unit. <cstring>, <algorithm>,
// <fstream>, <sstream> are already in scope from d3d12_replay.cpp.

namespace {

using Backend = D3D12ReplayBackend;

// 8-byte format magic; the trailing digit tracks the on-disk encoding, independent of the semantic
// schema_version (which guards field-set changes).
constexpr char kReplayModelBinaryMagic[8] = {'D', '3', 'R', 'P', 'M', 'B', '1', '\0'};

// ----------------------------------------------------------------------------
// Sequential binary stream primitives
// ----------------------------------------------------------------------------

// Append-only writer over an in-memory buffer. Native-endian raw writes only.
struct BinWriter {
  std::string buffer;

  template <typename T>
  void pod(const T &value)
  {
    buffer.append(reinterpret_cast<const char *>(&value), sizeof(T));
  }

  void u64(std::uint64_t value) { pod(value); }

  void str(const std::string &text)
  {
    u64(static_cast<std::uint64_t>(text.size()));
    buffer.append(text.data(), text.size());
  }
};

// Sequential reader over a fixed buffer. Every read is bounds-checked; on the first failure the
// reader latches an error and all further reads are no-ops so callers can check once at the end.
struct BinReader {
  const char *data = nullptr;
  std::size_t size = 0;
  std::size_t cursor = 0;
  bool ok = true;
  std::string error;

  bool fail(const char *why)
  {
    if (ok) {
      ok = false;
      error = why;
    }
    return false;
  }

  bool need(std::size_t count)
  {
    if (!ok) {
      return false;
    }
    if (count > size - cursor) {
      return fail("replay-model binary stream truncated");
    }
    return true;
  }

  template <typename T>
  void pod(T &out)
  {
    if (!need(sizeof(T))) {
      return;
    }
    std::memcpy(&out, data + cursor, sizeof(T));
    cursor += sizeof(T);
  }

  std::uint64_t u64()
  {
    std::uint64_t value = 0;
    pod(value);
    return value;
  }

  void str(std::string &out)
  {
    const std::uint64_t len = u64();
    if (!need(static_cast<std::size_t>(len))) {
      return;
    }
    out.assign(data + cursor, static_cast<std::size_t>(len));
    cursor += static_cast<std::size_t>(len);
  }

  // Guards a container count against the bytes physically remaining: every element needs at least
  // one byte, so a count larger than what is left is corruption (and avoids a huge reserve()).
  bool count_within_remaining(std::uint64_t count)
  {
    if (!ok) {
      return false;
    }
    if (count > size - cursor) {
      return fail("replay-model binary container count exceeds remaining bytes");
    }
    return true;
  }
};

// ----------------------------------------------------------------------------
// Binary blob pool (bulk byte arrays + large strings), referenced inline by {off,len}
// ----------------------------------------------------------------------------

// Append-only pool accumulating the ".bin" file; hands back the {offset,length} of each slice.
struct BlobWriter {
  std::string buffer;

  void append(const std::uint8_t *bytes, std::size_t len, std::uint64_t &out_off, std::uint64_t &out_len)
  {
    out_off = static_cast<std::uint64_t>(buffer.size());
    out_len = static_cast<std::uint64_t>(len);
    if (len != 0 && bytes != nullptr) {
      buffer.append(reinterpret_cast<const char *>(bytes), len);
    }
  }
};

// Read-only view over the loaded pool; slices it by {off,len} with bounds checks.
struct BlobReader {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
};

// ----------------------------------------------------------------------------
// Archives: a single serialize_fields(Archive&, T&) per struct drives both directions.
// ----------------------------------------------------------------------------

struct WriteArchive {
  BinWriter &bin;
  BlobWriter &blobs;
  static constexpr bool reading = false;

  // Scalar / trivially-copyable field (includes enums and trace id typedefs).
  template <typename T>
  void scalar(T &value) { bin.pod(value); }

  void string(std::string &value) { bin.str(value); }

  void path(std::filesystem::path &value)
  {
    const std::string text = value.generic_string();
    bin.str(text);
  }

  // Fixed C array of scalars.
  template <typename T, std::size_t N>
  void array(T (&values)[N])
  {
    for (std::size_t i = 0; i < N; ++i) {
      bin.pod(values[i]);
    }
  }

  // Vector of scalars.
  template <typename T>
  void scalar_vec(std::vector<T> &values)
  {
    bin.u64(static_cast<std::uint64_t>(values.size()));
    for (auto &value : values) {
      bin.pod(value);
    }
  }

  // Vector of structs (each via serialize_fields).
  template <typename T>
  void vec(std::vector<T> &values)
  {
    bin.u64(static_cast<std::uint64_t>(values.size()));
    for (auto &value : values) {
      serialize_fields(*this, value);
    }
  }

  // unordered_map<K, struct V>: emit count + {K,V} pairs sorted by key.
  template <typename K, typename V>
  void map(std::unordered_map<K, V> &m)
  {
    std::vector<K> keys;
    keys.reserve(m.size());
    for (auto &pair : m) {
      keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());
    bin.u64(static_cast<std::uint64_t>(keys.size()));
    for (const auto &k : keys) {
      K key = k;
      bin.pod(key);
      serialize_fields(*this, m.at(k));
    }
  }

  // unordered_map<K, scalar V>: same framing, scalar value.
  template <typename K, typename V>
  void scalar_map(std::unordered_map<K, V> &m)
  {
    std::vector<K> keys;
    keys.reserve(m.size());
    for (auto &pair : m) {
      keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());
    bin.u64(static_cast<std::uint64_t>(keys.size()));
    for (const auto &k : keys) {
      K key = k;
      bin.pod(key);
      V value = m.at(k);
      bin.pod(value);
    }
  }

  // Bulk byte vector -> blob pool, inline {off,len}.
  void blob_bytes(std::vector<std::uint8_t> &bytes)
  {
    std::uint64_t off = 0;
    std::uint64_t len = 0;
    blobs.append(bytes.data(), bytes.size(), off, len);
    bin.u64(off);
    bin.u64(len);
  }

  // Large string -> blob pool, inline {off,len}.
  void blob_string(std::string &text)
  {
    std::uint64_t off = 0;
    std::uint64_t len = 0;
    blobs.append(reinterpret_cast<const std::uint8_t *>(text.data()), text.size(), off, len);
    bin.u64(off);
    bin.u64(len);
  }
};

struct ReadArchive {
  BinReader &bin;
  const BlobReader &blobs;
  static constexpr bool reading = true;

  template <typename T>
  void scalar(T &value) { bin.pod(value); }

  void string(std::string &value) { bin.str(value); }

  void path(std::filesystem::path &value)
  {
    std::string text;
    bin.str(text);
    value = std::filesystem::path(text);
  }

  template <typename T, std::size_t N>
  void array(T (&values)[N])
  {
    for (std::size_t i = 0; i < N; ++i) {
      bin.pod(values[i]);
    }
  }

  template <typename T>
  void scalar_vec(std::vector<T> &values)
  {
    values.clear();
    const std::uint64_t count = bin.u64();
    if (!bin.count_within_remaining(count)) {
      return;
    }
    values.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count && bin.ok; ++i) {
      T value{};
      bin.pod(value);
      values.push_back(value);
    }
  }

  template <typename T>
  void vec(std::vector<T> &values)
  {
    values.clear();
    const std::uint64_t count = bin.u64();
    if (!bin.count_within_remaining(count)) {
      return;
    }
    values.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count && bin.ok; ++i) {
      T value{};
      serialize_fields(*this, value);
      values.push_back(std::move(value));
    }
  }

  template <typename K, typename V>
  void map(std::unordered_map<K, V> &m)
  {
    m.clear();
    const std::uint64_t count = bin.u64();
    if (!bin.count_within_remaining(count)) {
      return;
    }
    for (std::uint64_t i = 0; i < count && bin.ok; ++i) {
      K key{};
      bin.pod(key);
      V value{};
      serialize_fields(*this, value);
      m.emplace(key, std::move(value));
    }
  }

  template <typename K, typename V>
  void scalar_map(std::unordered_map<K, V> &m)
  {
    m.clear();
    const std::uint64_t count = bin.u64();
    if (!bin.count_within_remaining(count)) {
      return;
    }
    for (std::uint64_t i = 0; i < count && bin.ok; ++i) {
      K key{};
      bin.pod(key);
      V value{};
      bin.pod(value);
      m.emplace(key, value);
    }
  }

  void blob_bytes(std::vector<std::uint8_t> &bytes)
  {
    const std::uint64_t off = bin.u64();
    const std::uint64_t len = bin.u64();
    if (!bin.ok) {
      return;
    }
    if (off > blobs.size || len > blobs.size - off) {
      bin.fail("replay-model blob reference out of bounds");
      return;
    }
    bytes.assign(blobs.data + off, blobs.data + off + len);
  }

  void blob_string(std::string &text)
  {
    const std::uint64_t off = bin.u64();
    const std::uint64_t len = bin.u64();
    if (!bin.ok) {
      return;
    }
    if (off > blobs.size || len > blobs.size - off) {
      bin.fail("replay-model blob reference out of bounds");
      return;
    }
    text.assign(reinterpret_cast<const char *>(blobs.data + off), static_cast<std::size_t>(len));
  }
};

template <typename A>
void serialize_fields(A &a, Backend::GpuVirtualAddressBinding &v)
{
  a.scalar(v.gpu_virtual_address);
  a.scalar(v.resource_object_id);
  a.scalar(v.offset);
  a.scalar(v.resolved);
}

template <typename A>
void serialize_fields(A &a, Backend::DescriptorBinding &v)
{
  a.scalar(v.descriptor);
  a.scalar(v.heap_object_id);
  a.scalar(v.heap_type);
  a.scalar(v.descriptor_index);
}

template <typename A>
void serialize_fields(A &a, Backend::ViewportSemanticState &v)
{
  a.scalar(v.x);
  a.scalar(v.y);
  a.scalar(v.width);
  a.scalar(v.height);
  a.scalar(v.min_depth);
  a.scalar(v.max_depth);
}

template <typename A>
void serialize_fields(A &a, Backend::ScissorRectSemanticState &v)
{
  a.scalar(v.left);
  a.scalar(v.top);
  a.scalar(v.right);
  a.scalar(v.bottom);
}

template <typename A>
void serialize_fields(A &a, Backend::Root32BitConstantsBinding &v)
{
  a.scalar(v.root_parameter_index);
  a.scalar(v.dst_offset);
  a.scalar_vec(v.values);
}

template <typename A>
void serialize_fields(A &a, Backend::VertexBufferBinding &v)
{
  a.scalar(v.sequence);
  a.scalar(v.slot);
  serialize_fields(a, v.address);
  a.scalar(v.size_in_bytes);
  a.scalar(v.stride_in_bytes);
}

template <typename A>
void serialize_fields(A &a, Backend::IndexBufferBinding &v)
{
  a.scalar(v.sequence);
  serialize_fields(a, v.address);
  a.scalar(v.size_in_bytes);
  a.scalar(v.format);
}

template <typename A>
void serialize_fields(A &a, Backend::ResourceTransition &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.flags);
  a.scalar(v.before);
  a.scalar(v.after);
  a.scalar(v.subresource);
}

template <typename A>
void serialize_fields(A &a, Backend::ResourceBarrierSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.type);
  a.scalar(v.flags);
  a.scalar(v.resource_object_id);
  a.scalar(v.resource_before_object_id);
  a.scalar(v.resource_after_object_id);
  a.scalar(v.before);
  a.scalar(v.after);
  a.scalar(v.subresource);
}

template <typename A>
void serialize_fields(A &a, Backend::TextureCopyLocation &v)
{
  a.scalar(v.resource_object_id);
  a.scalar(v.type);
  a.scalar(v.subresource_index);
  a.scalar(v.footprint_offset);
  a.scalar(v.footprint_format);
  a.scalar(v.footprint_width);
  a.scalar(v.footprint_height);
  a.scalar(v.footprint_depth);
  a.scalar(v.footprint_row_pitch);
}

template <typename A>
void serialize_fields(A &a, Backend::TextureCopySemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  serialize_fields(a, v.dst);
  serialize_fields(a, v.src);
  a.scalar(v.dst_x);
  a.scalar(v.dst_y);
  a.scalar(v.dst_z);
  a.scalar(v.has_src_box);
  a.scalar(v.src_box_left);
  a.scalar(v.src_box_top);
  a.scalar(v.src_box_front);
  a.scalar(v.src_box_right);
  a.scalar(v.src_box_bottom);
  a.scalar(v.src_box_back);
}

template <typename A>
void serialize_fields(A &a, Backend::ResourceCopySemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.dst_resource_object_id);
  a.scalar(v.src_resource_object_id);
}

template <typename A>
void serialize_fields(A &a, Backend::BufferCopySemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.dst_buffer_object_id);
  a.scalar(v.src_buffer_object_id);
  a.scalar(v.dst_offset);
  a.scalar(v.src_offset);
  a.scalar(v.byte_count);
}

template <typename A>
void serialize_fields(A &a, Backend::ResolveSubresourceSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.dst_resource_object_id);
  a.scalar(v.src_resource_object_id);
  a.scalar(v.dst_subresource);
  a.scalar(v.dst_x);
  a.scalar(v.dst_y);
  a.scalar(v.src_subresource);
  a.scalar(v.has_src_rect);
  a.scalar(v.src_rect_left);
  a.scalar(v.src_rect_top);
  a.scalar(v.src_rect_right);
  a.scalar(v.src_rect_bottom);
  a.scalar(v.format);
  a.scalar(v.mode);
}

template <typename A>
void serialize_fields(A &a, Backend::QueryCommandSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.query_heap_object_id);
  a.scalar(v.type);
  a.scalar(v.index);
  a.scalar(v.start_index);
  a.scalar(v.query_count);
  a.scalar(v.dst_buffer_object_id);
  a.scalar(v.aligned_dst_buffer_offset);
  a.scalar(v.resolve);
  a.scalar(v.end);
}

template <typename A>
void serialize_fields(A &a, Backend::PredicationSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.buffer_object_id);
  a.scalar(v.aligned_buffer_offset);
  a.scalar(v.operation);
}

template <typename A>
void serialize_fields(A &a, Backend::WriteBufferImmediateEntry &v)
{
  serialize_fields(a, v.dest);
  a.scalar(v.value);
  a.scalar(v.mode);
}

template <typename A>
void serialize_fields(A &a, Backend::WriteBufferImmediateSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.vec(v.writes);
}

template <typename A>
void serialize_fields(A &a, Backend::RenderTargetClearSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  serialize_fields(a, v.render_target);
  a.array(v.color);
  a.scalar(v.rect_count);
  a.scalar(v.has_color);
  a.vec(v.rects);
}

template <typename A>
void serialize_fields(A &a, Backend::DepthStencilClearSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  serialize_fields(a, v.depth_stencil);
  a.scalar(v.depth);
  a.scalar(v.clear_flags);
  a.scalar(v.stencil);
  a.scalar(v.rect_count);
  a.vec(v.rects);
}

template <typename A>
void serialize_fields(A &a, Backend::UnorderedAccessClearSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  serialize_fields(a, v.gpu_descriptor);
  serialize_fields(a, v.cpu_descriptor);
  a.scalar(v.resource_object_id);
  a.array(v.uint_values);
  a.array(v.float_values);
  a.scalar(v.integer);
  a.scalar(v.rect_count);
  a.vec(v.rects);
}

template <typename A>
void serialize_fields(A &a, Backend::DiscardResourceSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.resource_object_id);
  a.scalar(v.first_subresource);
  a.scalar(v.subresource_count);
  a.scalar(v.rect_count);
  a.scalar(v.has_region);
  a.vec(v.rects);
}

template <typename A>
void serialize_fields(A &a, Backend::CommandSignatureArgument &v)
{
  a.scalar(v.type);
  a.scalar(v.slot);
  a.scalar(v.root_parameter_index);
  a.scalar(v.dest_offset_in32bit_values);
  a.scalar(v.num32bit_values_to_set);
}

template <typename A>
void serialize_fields(A &a, Backend::CommandSignatureSemanticState &v)
{
  a.scalar(v.command_signature_object_id);
  a.scalar(v.root_signature_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.byte_stride);
  a.scalar(v.argument_count);
  a.scalar(v.node_mask);
  a.vec(v.arguments);
}

template <typename A>
void serialize_fields(A &a, Backend::ExecuteIndirectSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.command_signature_object_id);
  a.scalar(v.arg_buffer_object_id);
  a.scalar(v.count_buffer_object_id);
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.graphics_root_signature_object_id);
  a.scalar(v.compute_root_signature_object_id);
  a.map(v.graphics_root_tables);
  a.map(v.compute_root_tables);
  a.vec(v.render_targets);
  serialize_fields(a, v.depth_stencil);
  a.vec(v.viewports);
  a.vec(v.scissor_rects);
  a.scalar(v.primitive_topology);
  a.scalar(v.max_command_count);
  a.scalar(v.arg_buffer_offset);
  a.scalar(v.count_buffer_offset);
}

template <typename A>
void serialize_fields(A &a, Backend::ExecuteBundleSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.bundle_command_list_object_id);
}

template <typename A>
void serialize_fields(A &a, Backend::DispatchSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.graphics_root_signature_object_id);
  a.scalar(v.compute_root_signature_object_id);
  a.map(v.graphics_root_tables);
  a.map(v.compute_root_tables);
  a.scalar(v.thread_group_count_x);
  a.scalar(v.thread_group_count_y);
  a.scalar(v.thread_group_count_z);
  a.scalar(v.mesh);
}

template <typename A>
void serialize_fields(A &a, Backend::DispatchRaysSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.width);
  a.scalar(v.height);
  a.scalar(v.depth);
  serialize_fields(a, v.ray_generation_shader_record);
  a.scalar(v.ray_generation_shader_record_size);
  serialize_fields(a, v.miss_shader_table);
  a.scalar(v.miss_shader_table_size);
  a.scalar(v.miss_shader_table_stride);
  serialize_fields(a, v.hit_group_table);
  a.scalar(v.hit_group_table_size);
  a.scalar(v.hit_group_table_stride);
  serialize_fields(a, v.callable_shader_table);
  a.scalar(v.callable_shader_table_size);
  a.scalar(v.callable_shader_table_stride);
}

template <typename A>
void serialize_fields(A &a, Backend::TemporalUpscaleSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.input_content_width);
  a.scalar(v.input_content_height);
  a.scalar(v.auto_exposure);
  a.scalar(v.in_reset);
  a.scalar(v.depth_reversed);
  a.scalar(v.motion_vector_in_display_res);
  a.scalar(v.color_object_id);
  a.scalar(v.depth_object_id);
  a.scalar(v.motion_vector_object_id);
  a.scalar(v.output_object_id);
  a.scalar(v.exposure_texture_object_id);
  a.scalar(v.motion_vector_scale_x);
  a.scalar(v.motion_vector_scale_y);
  a.scalar(v.pre_exposure);
  a.scalar(v.jitter_offset_x);
  a.scalar(v.jitter_offset_y);
}

template <typename A>
void serialize_fields(A &a, Backend::DrawSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.graphics_root_signature_object_id);
  a.map(v.graphics_root_tables);
  a.vec(v.render_targets);
  serialize_fields(a, v.depth_stencil);
  a.vec(v.viewports);
  a.vec(v.scissor_rects);
  a.scalar(v.primitive_topology);
  a.scalar(v.vertex_count_per_instance);
  a.scalar(v.instance_count);
  a.scalar(v.start_vertex_location);
  a.scalar(v.start_instance_location);
}

template <typename A>
void serialize_fields(A &a, Backend::DrawIndexedSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.graphics_root_signature_object_id);
  a.map(v.graphics_root_tables);
  a.vec(v.render_targets);
  serialize_fields(a, v.depth_stencil);
  a.vec(v.viewports);
  a.vec(v.scissor_rects);
  a.scalar(v.primitive_topology);
  a.scalar(v.index_count_per_instance);
  a.scalar(v.instance_count);
  a.scalar(v.start_index_location);
  a.scalar(v.base_vertex_location);
  a.scalar(v.start_instance_location);
}

template <typename A>
void serialize_fields(A &a, Backend::RootDescriptorRangeSemanticState &v)
{
  a.scalar(v.type);
  a.scalar(v.descriptor_count);
  a.scalar(v.base_shader_register);
  a.scalar(v.register_space);
  a.scalar(v.offset_from_table_start);
  a.scalar(v.flags);
}

template <typename A>
void serialize_fields(A &a, Backend::RootDescriptorTableSemanticState &v)
{
  a.scalar(v.root_parameter_index);
  a.scalar(v.shader_visibility);
  a.vec(v.ranges);
}

template <typename A>
void serialize_fields(A &a, Backend::RootParameterSemanticState &v)
{
  a.scalar(v.root_parameter_index);
  a.scalar(v.parameter_type);
  a.scalar(v.shader_visibility);
  a.scalar(v.shader_register);
  a.scalar(v.register_space);
  a.scalar(v.num_32bit_values);
  a.scalar(v.range_count);
  a.scalar(v.flags);
}

template <typename A>
void serialize_fields(A &a, Backend::DescriptorSemanticState &v)
{
  a.string(v.kind);  // small inline string
  a.scalar(v.create_sequence);
  a.scalar(v.resource_object_id);
  a.scalar(v.counter_resource_object_id);
  a.scalar(v.descriptor);
  serialize_fields(a, v.binding);
  a.scalar(v.format);
  a.scalar(v.view_dimension);
  a.scalar(v.flags);
  a.scalar(v.shader_4_component_mapping);
  a.scalar(v.buffer_location);
  a.scalar(v.size_in_bytes);
  a.scalar(v.first_element);
  a.scalar(v.num_elements);
  a.scalar(v.structure_byte_stride);
  a.scalar(v.counter_offset_in_bytes);
  a.scalar(v.has_view_desc);
  a.scalar(v.most_detailed_mip);
  a.scalar(v.mip_levels);
  a.scalar(v.mip_slice);
  a.scalar(v.plane_slice);
  a.scalar(v.first_array_slice);
  a.scalar(v.array_size);
  a.scalar(v.first_2d_array_face);
  a.scalar(v.num_cubes);
  a.scalar(v.first_w_slice);
  a.scalar(v.w_size);
  a.scalar(v.resource_min_lod_clamp);
  serialize_fields(a, v.raytracing_acceleration_structure);
  serialize_fields(a, v.buffer);
}

template <typename A>
void serialize_fields(A &a, Backend::SamplerSemanticState &v)
{
  a.scalar(v.create_sequence);
  a.scalar(v.descriptor);
  serialize_fields(a, v.binding);
  a.scalar(v.filter);
  a.scalar(v.address_u);
  a.scalar(v.address_v);
  a.scalar(v.address_w);
  a.scalar(v.mip_lod_bias);
  a.scalar(v.max_anisotropy);
  a.scalar(v.comparison_func);
  a.array(v.border_color);
  a.scalar(v.min_lod);
  a.scalar(v.max_lod);
  a.scalar(v.has_desc);
}

template <typename A>
void serialize_fields(A &a, Backend::CopyDescriptorPair &v)
{
  a.scalar(v.dst_descriptor);
  a.scalar(v.src_descriptor);
}

template <typename A>
void serialize_fields(A &a, Backend::CopyDescriptorSemanticState &v)
{
  a.scalar(v.create_sequence);
  a.scalar(v.descriptor_heap_type);
  a.vec(v.descriptors);
}

template <typename A>
void serialize_fields(A &a, Backend::DeviceSemanticState &v)
{
  a.scalar(v.device_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.minimum_feature_level);
}

template <typename A>
void serialize_fields(A &a, Backend::CommandQueueSemanticState &v)
{
  a.scalar(v.queue_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.type);
  a.scalar(v.priority);
  a.scalar(v.flags);
  a.scalar(v.node_mask);
  a.scalar(v.execute_count);
  a.scalar(v.last_execute_sequence);
}

template <typename A>
void serialize_fields(A &a, Backend::CommandAllocatorSemanticState &v)
{
  a.scalar(v.allocator_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.type);
  a.scalar(v.reset_count);
  a.scalar(v.last_reset_sequence);
  a.scalar(v.last_submit_sequence);
  a.scalar(v.last_submit_fence_object_id);
  a.scalar(v.last_submit_fence_value);
}

template <typename A>
void serialize_fields(A &a, Backend::HeapSemanticState &v)
{
  a.scalar(v.heap_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.size_in_bytes);
  a.scalar(v.alignment);
  a.scalar(v.heap_type);
  a.scalar(v.cpu_page_property);
  a.scalar(v.memory_pool_preference);
  a.scalar(v.creation_node_mask);
  a.scalar(v.visible_node_mask);
  a.scalar(v.flags);
}

template <typename A>
void serialize_fields(A &a, Backend::QueryHeapSemanticState &v)
{
  a.scalar(v.query_heap_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.type);
  a.scalar(v.count);
  a.scalar(v.node_mask);
}

template <typename A>
void serialize_fields(A &a, Backend::DescriptorHeapSemanticState &v)
{
  a.scalar(v.heap_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.type);
  a.scalar(v.num_descriptors);
  a.scalar(v.flags);
  a.scalar(v.node_mask);
  a.scalar(v.descriptor_size);
  a.scalar(v.cpu_start);
  a.scalar(v.gpu_start);
}

template <typename A>
void serialize_fields(A &a, Backend::FenceSemanticState &v)
{
  a.scalar(v.fence_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.initial_value);
  a.scalar(v.flags);
  a.scalar(v.current_value);
  a.scalar(v.completed_value);
  a.scalar(v.last_signal_sequence);
  a.scalar(v.last_wait_sequence);
  a.scalar(v.last_event_sequence);
}

template <typename A>
void serialize_fields(A &a, Backend::FenceOperationSemanticState &v)
{
  a.scalar(v.sequence);
  a.scalar(v.queue_object_id);
  a.scalar(v.fence_object_id);
  a.scalar(v.fence_value);
  a.scalar(v.queue_operation);
  a.scalar(v.wait_operation);
  a.scalar(v.event_operation);
}

template <typename A>
void serialize_fields(A &a, Backend::PipelineSemanticState &v)
{
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.root_signature_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.graphics);
  a.path(v.relative_path);
  a.scalar_vec(v.blob_refs);
  a.scalar(v.uses_embedded_root_signature);
  a.scalar(v.node_mask);
  a.scalar(v.flags);
  a.scalar(v.sample_mask);
  a.scalar(v.primitive_topology_type);
  a.scalar(v.num_render_targets);
  a.scalar(v.dsv_format);
  a.scalar(v.sample_count);
  a.scalar(v.sample_quality);
  a.scalar(v.ib_strip_cut_value);
  a.scalar(v.input_element_count);
  a.scalar(v.has_input_layout);
  a.scalar(v.has_blend_state);
  a.scalar(v.has_rasterizer_state);
  a.scalar(v.has_depth_stencil_state);
  a.scalar_vec(v.rtv_formats);
  a.scalar(v.has_vertex_shader);
  a.scalar(v.has_pixel_shader);
  a.scalar(v.has_domain_shader);
  a.scalar(v.has_hull_shader);
  a.scalar(v.has_geometry_shader);
  a.scalar(v.has_compute_shader);
  a.scalar(v.requires_dxmt_backend);
}

template <typename A>
void serialize_fields(A &a, Backend::PresentFrame &v)
{
  a.path(v.relative_path);
  a.scalar(v.width);
  a.scalar(v.height);
  a.scalar(v.row_pitch);
  a.scalar(v.sync_interval);
  a.scalar(v.flags);
  a.scalar(v.frame_index);
}

template <typename A>
void serialize_fields(A &a, Backend::PresentSemanticState &v)
{
  a.scalar(v.frame_index);
  a.scalar(v.call_sequence);
  a.scalar(v.boundary_sequence);
  a.scalar(v.sync_interval);
  a.scalar(v.flags);
  a.scalar(v.result_code);
  a.scalar(v.has_call);
  a.scalar(v.has_boundary);
}

template <typename A>
void serialize_fields(A &a, Backend::FrameSemanticState &v)
{
  a.scalar(v.frame_index);
  a.scalar(v.begin_sequence);
  a.scalar(v.end_sequence);
  a.scalar(v.has_begin);
  a.scalar(v.has_present);
  a.scalar(v.has_end);
}

// Submission tracker types (apitrace::d3d12 namespace, outside the backend).
template <typename A>
void serialize_fields(A &a, D3D12QueueWait &v)
{
  a.scalar(v.queue_object_id);
  a.scalar(v.fence_object_id);
  a.scalar(v.fence_value);
  a.scalar(v.sequence);
}

template <typename A>
void serialize_fields(A &a, D3D12SubmissionBatch &v)
{
  a.scalar(v.queue_object_id);
  a.scalar(v.command_allocator_object_id);
  a.scalar_vec(v.command_list_ids);
  a.scalar_vec(v.descriptor_heap_ids);
  a.vec(v.waits_before_execute);
  a.scalar(v.fence_object_id);
  a.scalar(v.execute_sequence);
  a.scalar(v.fence_sequence);
  a.scalar(v.present_sequence);
  a.scalar(v.present_frame_index);
  a.scalar(v.fence_value);
  a.scalar(v.presented);
}

template <typename A>
void serialize_fields(A &a, Backend::CommandListSemanticState &v)
{
  a.scalar(v.recording);
  a.scalar(v.type);
  a.scalar(v.node_mask);
  a.scalar(v.allocator_object_id);
  a.scalar(v.pipeline_state_object_id);
  a.scalar(v.graphics_root_signature_object_id);
  a.scalar(v.compute_root_signature_object_id);
  a.scalar(v.descriptor_heap_count);
  a.scalar(v.render_target_count);
  a.scalar(v.barrier_count);
  a.scalar(v.clear_count);
  a.scalar(v.copy_count);
  a.scalar(v.draw_count);
  a.scalar(v.dispatch_count);
  a.scalar(v.first_sequence);
  a.scalar(v.close_sequence);
  a.scalar(v.primitive_topology);
  a.scalar_vec(v.descriptor_heap_object_ids);
  a.vec(v.viewports);
  a.vec(v.scissor_rects);
  a.vec(v.render_targets);
  serialize_fields(a, v.depth_stencil);
  a.map(v.graphics_root_tables);
  a.map(v.compute_root_tables);
  a.map(v.graphics_root_constants);
  a.map(v.compute_root_constants);
  a.map(v.graphics_root_constant_buffers);
  a.map(v.compute_root_constant_buffers);
  a.map(v.graphics_root_shader_resources);
  a.map(v.compute_root_shader_resources);
  a.map(v.graphics_root_unordered_accesses);
  a.map(v.compute_root_unordered_accesses);
  a.vec(v.vertex_buffers);
  serialize_fields(a, v.index_buffer);
  a.vec(v.buffer_copies);
  a.vec(v.texture_copies);
  a.vec(v.resource_copies);
  a.vec(v.barriers);
  a.vec(v.resolves);
  a.vec(v.queries);
  a.vec(v.predications);
  a.vec(v.write_buffer_immediates);
  a.vec(v.render_target_clears);
  a.vec(v.depth_stencil_clears);
  a.vec(v.unordered_access_clears);
  a.vec(v.discards);
  a.vec(v.indirect_executes);
  a.vec(v.bundle_executes);
  a.vec(v.draws);
  a.vec(v.indexed_draws);
  a.vec(v.dispatches);
  a.vec(v.ray_dispatches);
  a.vec(v.temporal_upscales);
  a.scalar_vec(v.command_indices);
}

// ----------------------------------------------------------------------------
// Blob-bearing structs (bulk byte arrays / large strings -> blob pool)
// ----------------------------------------------------------------------------

template <typename A>
void serialize_fields(A &a, Backend::ResourceDataUpdate &v)
{
  a.scalar(v.sequence);
  a.scalar(v.subresource);
  a.scalar(v.written_begin);
  a.scalar(v.written_end);
  a.path(v.relative_path);
  a.scalar_vec(v.blob_refs);
  a.blob_bytes(v.bytes);
}

template <typename A>
void serialize_fields(A &a, Backend::ResourceSemanticState &v)
{
  a.scalar(v.resource_object_id);
  a.scalar(v.heap_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.heap_type);
  a.scalar(v.heap_flags);
  a.scalar(v.initial_state);
  a.scalar(v.current_state);
  a.scalar(v.heap_offset);
  a.scalar(v.last_transition_sequence);
  a.scalar(v.gpu_virtual_address);
  a.scalar(v.width);
  a.scalar(v.alignment);
  a.scalar(v.dimension);
  a.scalar(v.height);
  a.scalar(v.depth_or_array_size);
  a.scalar(v.mip_levels);
  a.scalar(v.recorded_mip_levels);
  a.scalar(v.format);
  a.scalar(v.sample_count);
  a.scalar(v.sample_quality);
  a.scalar(v.layout);
  a.scalar(v.flags);
  a.scalar(v.has_optimized_clear_value);
  a.scalar(v.reserved_resource);
  a.scalar(v.swapchain_back_buffer);
  a.scalar(v.external_resource);
  a.scalar(v.swapchain_buffer_index);
  a.scalar(v.optimized_clear_format);
  a.array(v.optimized_clear_color);
  a.scalar(v.optimized_clear_depth);
  a.scalar(v.optimized_clear_stencil);
  a.scalar(v.mapped);
  a.scalar(v.mapped_subresource);
  a.scalar(v.map_sequence);
  a.vec(v.transitions);
  a.scalar_map(v.subresource_states);
  a.vec(v.data_updates);
}

template <typename A>
void serialize_fields(A &a, Backend::RootSignatureSemanticState &v)
{
  a.scalar(v.root_signature_object_id);
  a.scalar(v.create_sequence);
  a.scalar(v.node_mask);
  a.scalar(v.bytecode_size);
  a.path(v.relative_path);
  a.scalar_vec(v.blob_refs);
  a.blob_bytes(v.bytes);
  a.vec(v.descriptor_tables);
  a.scalar(v.has_descriptor_table_metadata);
  a.vec(v.root_parameters);
  a.scalar(v.has_root_parameter_metadata);
}

template <typename A>
void serialize_fields(A &a, Backend::ReplayCommandRecord &v)
{
  a.scalar(v.kind);
  a.scalar(v.sequence);
  a.scalar(v.command_list_object_id);
  a.scalar_vec(v.object_refs);
  a.blob_string(v.function_name);
  a.blob_string(v.payload);
}

template <typename A>
void serialize_fields(A &a, Backend::TileMappingUpdateRecord &v)
{
  a.scalar(v.sequence);
  a.scalar(v.queue_object_id);
  a.scalar(v.resource_object_id);
  a.scalar(v.heap_object_id);
  a.blob_string(v.payload);
}

// D3D12ObjectRegistry tracked object record.
template <typename A>
void serialize_fields(A &a, D3D12TrackedObject &v)
{
  a.scalar(v.object_id);
  a.scalar(v.kind);
  a.scalar(v.parent_object_id);
  a.string(v.debug_name);
}

} // namespace

bool D3D12ReplayBackend::save_replay_model(
    const std::filesystem::path &json_path,
    const std::filesystem::path &blob_path,
    const std::string &source_bundle_hash,
    std::string &error) const
{
  // WriteArchive only reads from the model; const_cast lets the shared (non-const) archive API drive
  // the const save path without duplicating every helper for const containers.
  Backend &self = const_cast<Backend &>(*this);

  BinWriter body;
  BlobWriter blobs;
  WriteArchive a{body, blobs};

  // Stats counters.
  a.scalar(self.commands_replayed_);
  a.scalar(self.frames_seen_);
  a.scalar(self.presents_seen_);
  a.scalar(self.pipeline_assets_read_);
  a.scalar(self.semantic_calls_seen_);
  a.scalar(self.draw_calls_seen_);
  a.scalar(self.dispatch_calls_seen_);
  a.scalar(self.last_sequence_);

  a.path(self.bundle_root_);

  // Object registry, sorted by object id for determinism.
  {
    const auto &tracked = self.objects_.tracked_objects();
    std::vector<trace::ObjectId> keys;
    keys.reserve(tracked.size());
    for (const auto &pair : tracked) {
      keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());
    body.u64(static_cast<std::uint64_t>(keys.size()));
    for (const auto key : keys) {
      D3D12TrackedObject object = tracked.at(key);
      serialize_fields(a, object);
    }
  }

  // Submission tracker.
  {
    std::vector<D3D12SubmissionBatch> completed_batches = self.submissions_.completed_batches();
    a.vec(completed_batches);
    bool has_open_batch = self.submissions_.has_open_batch();
    a.scalar(has_open_batch);
    D3D12SubmissionBatch current_batch = self.submissions_.current_batch();
    serialize_fields(a, current_batch);
    std::vector<D3D12QueueWait> pending_waits = self.submissions_.pending_waits();
    a.vec(pending_waits);
  }

  // blob_paths_ (BlobId -> path), sorted by blob id.
  {
    std::vector<trace::BlobId> keys;
    keys.reserve(self.blob_paths_.size());
    for (const auto &pair : self.blob_paths_) {
      keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());
    body.u64(static_cast<std::uint64_t>(keys.size()));
    for (const auto key : keys) {
      trace::BlobId id = key;
      body.pod(id);
      std::string text = self.blob_paths_.at(key).generic_string();
      body.str(text);
    }
  }

  // Pure-value map containers.
  a.map(self.present_frames_);
  a.map(self.present_semantics_);
  a.map(self.frame_semantics_);
  a.map(self.command_lists_);
  a.map(self.command_queues_);
  a.map(self.command_allocators_);
  a.map(self.devices_);
  a.map(self.heaps_);
  a.map(self.query_heaps_);
  a.map(self.descriptor_heaps_);
  a.map(self.command_signatures_);
  a.map(self.fences_);

  // Blob-bearing maps (resources carry data-update bytes; serialize_fields routes them to the pool).
  a.map(self.resources_);
  // Append-only per-create history of resources (object_id pointer reuse: a descriptor/command from
  // an earlier resource lifetime must resolve to the version live at its own sequence). Same element
  // serializer as resources_' values, including bytes/transitions/data_updates.
  a.vec(self.resource_versions_);
  a.map(self.pipelines_);
  a.map(self.root_signatures_);

  // Ordered vector containers.
  a.vec(self.descriptors_);
  a.vec(self.samplers_);
  a.vec(self.descriptor_copies_);
  a.vec(self.fence_operations_);

  // Blob-bearing vectors.
  a.vec(self.replay_commands_);
  a.vec(self.tile_mapping_updates_);

  // Assemble the structural file: header + body. blob_size is known only after the body is built.
  BinWriter head;
  head.buffer.append(kReplayModelBinaryMagic, sizeof(kReplayModelBinaryMagic));
  std::uint32_t version = kReplayModelSchemaVersion;
  head.pod(version);
  std::string hash = source_bundle_hash;
  head.str(hash);
  head.u64(static_cast<std::uint64_t>(blobs.buffer.size()));

  // Write the blob pool first so a present structural file always has its companion blob.
  {
    std::ofstream blob_out(blob_path, std::ios::binary | std::ios::trunc);
    if (!blob_out.is_open()) {
      error = "failed to open replay-model blob file for writing: " + blob_path.string();
      return false;
    }
    if (!blobs.buffer.empty()) {
      blob_out.write(blobs.buffer.data(), static_cast<std::streamsize>(blobs.buffer.size()));
    }
    if (!blob_out.good()) {
      error = "failed to write replay-model blob file: " + blob_path.string();
      return false;
    }
  }
  {
    std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      error = "failed to open replay-model file for writing: " + json_path.string();
      return false;
    }
    out.write(head.buffer.data(), static_cast<std::streamsize>(head.buffer.size()));
    out.write(body.buffer.data(), static_cast<std::streamsize>(body.buffer.size()));
    if (!out.good()) {
      error = "failed to write replay-model file: " + json_path.string();
      return false;
    }
  }
  return true;
}

bool D3D12ReplayBackend::load_replay_model(
    const std::filesystem::path &json_path,
    const std::filesystem::path &blob_path,
    const std::string &expected_source_bundle_hash,
    std::string &error)
{
  // Slurp the structural file (one sequential read, no lexer/DOM).
  std::string structural;
  {
    std::ifstream in(json_path, std::ios::binary);
    if (!in.is_open()) {
      error = "failed to open replay-model file: " + json_path.string();
      return false;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    structural = contents.str();
  }

  BinReader head;
  head.data = structural.data();
  head.size = structural.size();

  // Magic.
  char magic[sizeof(kReplayModelBinaryMagic)] = {};
  for (auto &byte : magic) {
    head.pod(byte);
  }
  if (!head.ok || std::memcmp(magic, kReplayModelBinaryMagic, sizeof(magic)) != 0) {
    error = "replay-model file has bad magic (not a binary replay model)";
    return false;
  }

  // Schema version.
  std::uint32_t schema_version = 0;
  head.pod(schema_version);
  if (!head.ok) {
    error = "replay-model file truncated in header";
    return false;
  }
  if (schema_version != kReplayModelSchemaVersion) {
    error = "replay-model schema version mismatch (have " + std::to_string(schema_version) +
            ", expected " + std::to_string(kReplayModelSchemaVersion) + ")";
    return false;
  }

  // Source bundle hash + staleness guard.
  std::string stored_hash;
  head.str(stored_hash);
  std::uint64_t expected_blob_size = head.u64();
  if (!head.ok) {
    error = "replay-model file truncated in header";
    return false;
  }
  if (!expected_source_bundle_hash.empty() && stored_hash != expected_source_bundle_hash) {
    error = "stale replay model: source bundle hash mismatch (have '" + stored_hash +
            "', expected '" + expected_source_bundle_hash + "')";
    return false;
  }

  // Load the blob pool and sanity-check its size.
  std::string blob_buffer;
  {
    std::ifstream blob_in(blob_path, std::ios::binary);
    if (!blob_in.is_open()) {
      error = "failed to open replay-model blob file: " + blob_path.string();
      return false;
    }
    std::ostringstream contents;
    contents << blob_in.rdbuf();
    blob_buffer = contents.str();
  }
  if (blob_buffer.size() != expected_blob_size) {
    error = "replay-model blob size mismatch (file " + std::to_string(blob_buffer.size()) +
            ", header " + std::to_string(expected_blob_size) + ")";
    return false;
  }

  // The body reader continues from the header cursor over the same buffer.
  BinReader body;
  body.data = structural.data();
  body.size = structural.size();
  body.cursor = head.cursor;
  BlobReader blobs;
  blobs.data = reinterpret_cast<const std::uint8_t *>(blob_buffer.data());
  blobs.size = blob_buffer.size();
  ReadArchive a{body, blobs};

  // Reset all state before repopulating.
  shutdown();

  // Stats counters (mirror save order exactly).
  a.scalar(commands_replayed_);
  a.scalar(frames_seen_);
  a.scalar(presents_seen_);
  a.scalar(pipeline_assets_read_);
  a.scalar(semantic_calls_seen_);
  a.scalar(draw_calls_seen_);
  a.scalar(dispatch_calls_seen_);
  a.scalar(last_sequence_);

  a.path(bundle_root_);

  // Object registry.
  {
    const std::uint64_t count = body.u64();
    if (body.count_within_remaining(count)) {
      for (std::uint64_t i = 0; i < count && body.ok; ++i) {
        D3D12TrackedObject object;
        serialize_fields(a, object);
        objects_.track(object);
      }
    }
  }

  // Submission tracker.
  {
    std::vector<D3D12SubmissionBatch> completed_batches;
    a.vec(completed_batches);
    bool has_open_batch = false;
    a.scalar(has_open_batch);
    D3D12SubmissionBatch current_batch{};
    serialize_fields(a, current_batch);
    std::vector<D3D12QueueWait> pending_waits;
    a.vec(pending_waits);
    submissions_.restore(completed_batches, current_batch, pending_waits, has_open_batch);
  }

  // blob_paths_.
  {
    const std::uint64_t count = body.u64();
    if (body.count_within_remaining(count)) {
      for (std::uint64_t i = 0; i < count && body.ok; ++i) {
        trace::BlobId id = 0;
        body.pod(id);
        std::string text;
        body.str(text);
        blob_paths_[id] = std::filesystem::path(text);
      }
    }
  }

  // Pure-value map containers.
  a.map(present_frames_);
  a.map(present_semantics_);
  a.map(frame_semantics_);
  a.map(command_lists_);
  a.map(command_queues_);
  a.map(command_allocators_);
  a.map(devices_);
  a.map(heaps_);
  a.map(query_heaps_);
  a.map(descriptor_heaps_);
  a.map(command_signatures_);
  a.map(fences_);

  // Blob-bearing maps.
  a.map(resources_);
  a.vec(resource_versions_);
  a.map(pipelines_);
  a.map(root_signatures_);

  // Ordered vector containers.
  a.vec(descriptors_);
  a.vec(samplers_);
  a.vec(descriptor_copies_);
  a.vec(fence_operations_);

  // Blob-bearing vectors.
  a.vec(replay_commands_);
  a.vec(tile_mapping_updates_);

  if (!body.ok) {
    error = body.error.empty() ? "replay-model binary decode failed" : body.error;
    return false;
  }

  initialized_ = true;
  return true;
}









