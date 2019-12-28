#include "BParticles.h"
#include "simulate.hpp"
#include "world_state.hpp"
#include "simulation_state.hpp"
#include "node_frontend.hpp"

#include "BLI_timeit.h"
#include "BLI_string.h"

#include "BKE_mesh.h"
#include "BKE_customdata.h"
#include "BKE_inlined_node_tree.h"

#include "DEG_depsgraph_query.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#ifdef WITH_TBB
#  define TBB_SUPPRESS_DEPRECATED_MESSAGES 1
#  include "tbb/tbb.h"
#endif

#define WRAPPERS(T1, T2) \
  inline T1 unwrap(T2 value) \
  { \
    return (T1)value; \
  } \
  inline T2 wrap(T1 value) \
  { \
    return (T2)value; \
  }

using namespace BParticles;

using BLI::ArrayRef;
using BLI::float3;
using BLI::rgba_b;
using BLI::rgba_f;
using BLI::StringRef;
using BLI::Vector;

WRAPPERS(SimulationState *, BParticlesSimulationState)

BParticlesSimulationState BParticles_new_simulation()
{
  SimulationState *state = new SimulationState();
  return wrap(state);
}

void BParticles_simulation_free(BParticlesSimulationState state_c)
{
  delete unwrap(state_c);
}

void BParticles_simulate_modifier(BParticlesModifierData *bpmd,
                                  Depsgraph *UNUSED(depsgraph),
                                  BParticlesSimulationState state_c,
                                  float time_step)
{
  if (bpmd->node_tree == NULL) {
    return;
  }

  SimulationState &simulation_state = *unwrap(state_c);
  simulation_state.time().start_update(time_step);

  bNodeTree *btree = (bNodeTree *)DEG_get_original_id((ID *)bpmd->node_tree);
  auto simulator = simulator_from_node_tree(btree);

  simulator->simulate(simulation_state);

  simulation_state.time().end_update();

  auto &containers = simulation_state.particles().particle_containers();
  containers.foreach_key_value_pair(
      [](StringRefNull system_name, AttributesBlockContainer *container) {
        std::cout << "Particle System: " << system_name << "\n";
        std::cout << "  Particles: " << container->count_active() << "\n";
        std::cout << "  Blocks: " << container->active_blocks().size() << "\n";
      });
}

static float3 tetrahedon_vertices[4] = {
    {1, -1, -1},
    {1, 1, 1},
    {-1, -1, 1},
    {-1, 1, -1},
};

static uint tetrahedon_loop_starts[4] = {0, 3, 6, 9};
static uint tetrahedon_loop_lengths[4] = {3, 3, 3, 3};
static uint tetrahedon_loop_vertices[12] = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 2, 3};
static uint tetrahedon_loop_edges[12] = {0, 3, 1, 2, 4, 0, 1, 5, 2, 3, 5, 4};
static uint tetrahedon_edges[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};

static void distribute_tetrahedons_range(Mesh *mesh,
                                         MutableArrayRef<MLoopCol> loop_colors,
                                         IndexRange range,
                                         ArrayRef<float3> centers,
                                         ArrayRef<float> scales,
                                         ArrayRef<rgba_f> colors)
{
  for (uint instance : range) {
    uint vertex_offset = instance * ARRAY_SIZE(tetrahedon_vertices);
    uint face_offset = instance * ARRAY_SIZE(tetrahedon_loop_starts);
    uint loop_offset = instance * ARRAY_SIZE(tetrahedon_loop_vertices);
    uint edge_offset = instance * ARRAY_SIZE(tetrahedon_edges);

    float3 center = centers[instance];
    for (uint i = 0; i < ARRAY_SIZE(tetrahedon_vertices); i++) {
      copy_v3_v3(mesh->mvert[vertex_offset + i].co,
                 center + tetrahedon_vertices[i] * scales[instance]);
    }

    for (uint i = 0; i < ARRAY_SIZE(tetrahedon_loop_starts); i++) {
      mesh->mpoly[face_offset + i].loopstart = loop_offset + tetrahedon_loop_starts[i];
      mesh->mpoly[face_offset + i].totloop = tetrahedon_loop_lengths[i];
    }

    rgba_f color_f = colors[instance];
    rgba_b color_b = color_f;
    MLoopCol loop_col = {color_b.r, color_b.g, color_b.b, color_b.a};
    for (uint i = 0; i < ARRAY_SIZE(tetrahedon_loop_vertices); i++) {
      mesh->mloop[loop_offset + i].v = vertex_offset + tetrahedon_loop_vertices[i];
      mesh->mloop[loop_offset + i].e = edge_offset + tetrahedon_loop_edges[i];
      loop_colors[loop_offset + i] = loop_col;
    }

    for (uint i = 0; i < ARRAY_SIZE(tetrahedon_edges); i++) {
      mesh->medge[edge_offset + i].v1 = vertex_offset + tetrahedon_edges[i][0];
      mesh->medge[edge_offset + i].v2 = vertex_offset + tetrahedon_edges[i][1];
    }
  }
}

static Mesh *distribute_tetrahedons(ArrayRef<float3> centers,
                                    ArrayRef<float> scales,
                                    ArrayRef<rgba_f> colors)
{
  uint amount = centers.size();
  Mesh *mesh = BKE_mesh_new_nomain(amount * ARRAY_SIZE(tetrahedon_vertices),
                                   amount * ARRAY_SIZE(tetrahedon_edges),
                                   0,
                                   amount * ARRAY_SIZE(tetrahedon_loop_vertices),
                                   amount * ARRAY_SIZE(tetrahedon_loop_starts));

  auto loop_colors = MutableArrayRef<MLoopCol>(
      (MLoopCol *)CustomData_add_layer_named(
          &mesh->ldata, CD_MLOOPCOL, CD_DEFAULT, nullptr, mesh->totloop, "Color"),
      mesh->totloop);

#if WITH_TBB
  tbb::parallel_for(
      tbb::blocked_range<uint>(0, amount, 1000), [&](const tbb::blocked_range<uint> &range) {
        distribute_tetrahedons_range(mesh, loop_colors, range, centers, scales, colors);
      });
#else
  distribute_tetrahedons_range(mesh, loop_colors, IndexRange(amount), centers, scales, colors);
#endif

  return mesh;
}

static Mesh *distribute_points(ArrayRef<float3> points)
{
  Mesh *mesh = BKE_mesh_new_nomain(points.size(), 0, 0, 0, 0);

  for (uint i = 0; i < mesh->totvert; i++) {
    copy_v3_v3(mesh->mvert[i].co, points[i]);
    mesh->mvert[i].no[2] = 32767;
  }

  return mesh;
}

void BParticles_modifier_free_cache(BParticlesModifierData *bpmd)
{
  if (bpmd->cached_frames == nullptr) {
    BLI_assert(bpmd->num_cached_frames == 0);
    return;
  }

  for (auto &cached_frame : BLI::ref_c_array(bpmd->cached_frames, bpmd->num_cached_frames)) {
    for (auto &cached_type :
         BLI::ref_c_array(cached_frame.particle_types, cached_frame.num_particle_types)) {
      for (auto &cached_attribute :
           BLI::ref_c_array(cached_type.attributes_float, cached_type.num_attributes_float)) {
        if (cached_attribute.values != nullptr) {
          MEM_freeN(cached_attribute.values);
        }
      }
      if (cached_type.attributes_float != nullptr) {
        MEM_freeN(cached_type.attributes_float);
      }
    }
    if (cached_frame.particle_types != nullptr) {
      MEM_freeN(cached_frame.particle_types);
    }
  }
  MEM_freeN(bpmd->cached_frames);
  bpmd->cached_frames = nullptr;
  bpmd->num_cached_frames = 0;
}

Mesh *BParticles_modifier_point_mesh_from_state(BParticlesSimulationState state_c)
{
  SimulationState &state = *unwrap(state_c);

  Vector<float3> positions;
  state.particles().particle_containers().foreach_value(
      [&positions](AttributesBlockContainer *container) {
        positions.extend(container->flatten_attribute<float3>("Position"));
      });

  return distribute_points(positions);
}

Mesh *BParticles_modifier_mesh_from_state(BParticlesSimulationState state_c)
{
  SimulationState &state = *unwrap(state_c);

  Vector<float3> positions;
  Vector<float> sizes;
  Vector<rgba_f> colors;

  state.particles().particle_containers().foreach_value(
      [&positions, &colors, &sizes](AttributesBlockContainer *container) {
        positions.extend(container->flatten_attribute<float3>("Position"));
        colors.extend(container->flatten_attribute<rgba_f>("Color"));
        sizes.extend(container->flatten_attribute<float>("Size"));
      });

  Mesh *mesh = distribute_tetrahedons(positions, sizes, colors);
  return mesh;
}

Mesh *BParticles_modifier_mesh_from_cache(BParticlesFrameCache *cached_frame)
{
  Vector<float3> positions;
  Vector<float> sizes;
  Vector<rgba_f> colors;

  for (uint i = 0; i < cached_frame->num_particle_types; i++) {
    BParticlesTypeCache &type = cached_frame->particle_types[i];
    positions.extend(
        ArrayRef<float3>((float3 *)type.attributes_float[0].values, type.particle_amount));
    sizes.extend(ArrayRef<float>(type.attributes_float[1].values, type.particle_amount));
    colors.extend(
        ArrayRef<rgba_f>((rgba_f *)type.attributes_float[2].values, type.particle_amount));
  }

  Mesh *mesh = distribute_tetrahedons(positions, sizes, colors);
  return mesh;
}

Mesh *BParticles_state_extract_type__tetrahedons(BParticlesSimulationState simulation_state_c,
                                                 const char *particle_type)
{
  SimulationState &state = *unwrap(simulation_state_c);
  ParticlesState &particles = state.particles();
  AttributesBlockContainer **container_ptr = particles.particle_containers().lookup_ptr(
      particle_type);
  if (container_ptr == nullptr) {
    return BKE_mesh_new_nomain(0, 0, 0, 0, 0);
  }
  AttributesBlockContainer &container = **container_ptr;

  auto positions = container.flatten_attribute<float3>("Position");
  auto sizes = container.flatten_attribute<float>("Size");
  auto colors = container.flatten_attribute<rgba_f>("Color");

  return distribute_tetrahedons(positions, sizes, colors);
}

Mesh *BParticles_state_extract_type__points(BParticlesSimulationState simulation_state_c,
                                            const char *particle_type)
{
  SimulationState &state = *unwrap(simulation_state_c);
  ParticlesState &particles = state.particles();
  AttributesBlockContainer *container_ptr = particles.particle_containers().lookup_default(
      particle_type, nullptr);
  if (container_ptr == nullptr) {
    return BKE_mesh_new_nomain(0, 0, 0, 0, 0);
  }
  AttributesBlockContainer &container = *container_ptr;

  auto positions = container.flatten_attribute<float3>("Position");
  return distribute_points(positions);
}

void BParticles_modifier_cache_state(BParticlesModifierData *bpmd,
                                     BParticlesSimulationState state_c,
                                     float frame)
{
  SimulationState &state = *unwrap(state_c);

  Vector<std::string> container_names;
  Vector<AttributesBlockContainer *> containers;

  state.particles().particle_containers().foreach_key_value_pair(
      [&container_names, &containers](StringRefNull name, AttributesBlockContainer *container) {
        container_names.append(name);
        containers.append(container);
      });

  BParticlesFrameCache cached_frame;
  memset(&cached_frame, 0, sizeof(BParticlesFrameCache));
  cached_frame.frame = frame;
  cached_frame.num_particle_types = containers.size();
  cached_frame.particle_types = (BParticlesTypeCache *)MEM_calloc_arrayN(
      containers.size(), sizeof(BParticlesTypeCache), __func__);

  for (uint i : containers.index_iterator()) {
    AttributesBlockContainer &container = *containers[i];
    BParticlesTypeCache &cached_type = cached_frame.particle_types[i];

    strncpy(cached_type.name, container_names[i].data(), sizeof(cached_type.name) - 1);
    cached_type.particle_amount = container.count_active();

    cached_type.num_attributes_float = 3;
    cached_type.attributes_float = (BParticlesAttributeCacheFloat *)MEM_calloc_arrayN(
        cached_type.num_attributes_float, sizeof(BParticlesAttributeCacheFloat), __func__);

    BParticlesAttributeCacheFloat &position_attribute = cached_type.attributes_float[0];
    position_attribute.floats_per_particle = 3;
    strncpy(position_attribute.name, "Position", sizeof(position_attribute.name));
    position_attribute.values = (float *)MEM_malloc_arrayN(
        cached_type.particle_amount, sizeof(float3), __func__);
    container.flatten_attribute("Position",
                                FN::GenericMutableArrayRef(FN::CPP_TYPE<float3>(),
                                                           position_attribute.values,
                                                           cached_type.particle_amount));

    BParticlesAttributeCacheFloat &size_attribute = cached_type.attributes_float[1];
    size_attribute.floats_per_particle = 1;
    strncpy(size_attribute.name, "Size", sizeof(size_attribute.name));
    size_attribute.values = (float *)MEM_malloc_arrayN(
        cached_type.particle_amount, sizeof(float), __func__);
    container.flatten_attribute("Size",
                                FN::GenericMutableArrayRef(FN::CPP_TYPE<float>(),
                                                           size_attribute.values,
                                                           cached_type.particle_amount));

    BParticlesAttributeCacheFloat &color_attribute = cached_type.attributes_float[2];
    color_attribute.floats_per_particle = 4;
    strncpy(color_attribute.name, "Color", sizeof(color_attribute.name));
    color_attribute.values = (float *)MEM_malloc_arrayN(
        cached_type.particle_amount, sizeof(rgba_f), __func__);
    container.flatten_attribute("Color",
                                FN::GenericMutableArrayRef(FN::CPP_TYPE<rgba_f>(),
                                                           color_attribute.values,
                                                           cached_type.particle_amount));
  }

  bpmd->cached_frames = (BParticlesFrameCache *)MEM_reallocN(
      bpmd->cached_frames, sizeof(BParticlesFrameCache) * (bpmd->num_cached_frames + 1));
  bpmd->cached_frames[bpmd->num_cached_frames] = cached_frame;
  bpmd->num_cached_frames++;
}