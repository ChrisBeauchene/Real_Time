/* Copyright 2014 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <cstdio>
#include <cassert>
#include <cstdlib>
#include "../legion_runtime/legion.h"

#include "../legion_runtime/default_mapper.h"

using namespace LegionRuntime::HighLevel;
using namespace LegionRuntime::Accessor;
using namespace LegionRuntime::Arrays;

/*
 * In this example, we perform the same
 * daxpy computation as example 07.  While
 * the source code for the actual computation
 * between the two examples is identical, we 
 * show to create a custom mapper that changes 
 * the mapping of the computation onto the hardware.
 */

enum TaskIDs {
  TOP_LEVEL_TASK_ID,
  INIT_FIELD_TASK_ID,
  DAXPY_TASK_ID,
  CHECK_TASK_ID,
};

enum FieldIDs {
  FID_X,
  FID_Y,
  FID_Z,
};

enum {
  SUBREGION_TUNABLE,
};

enum {
  PARTITIONING_MAPPER_ID = 1,
};

/*
 * One of the primary goals of Legion is
 * to make it easy to remap applications
 * onto different hardware.  Up to this point
 * all of our applications have been mapped
 * by the DefaultMapper that we provide.  The
 * DefaultMapper class provides heuristics for
 * performing mappings that are good but often
 * not optimal for a specific application or
 * architecture.   By creating a custom mapper
 * programmers can make application- or
 * architecture-specific mapping decisions.
 * Furthermore, many custom mappers can be
 * used to map the same application onto many
 * different architectures without having to
 * modify the primary application code.
 *
 * A common concern when mapping applications
 * onto a target machine is whether or not
 * mapping impacts correctness.  In Legion
 * all mapping decisions are orthogonal to
 * correctness.  In cases when correctness may
 * be impacted by a mapping decision (e.g. a
 * mapper maps a physical region for a task
 * into a memory not visible from the target
 * processor), then the Legion runtime will
 * notify the mapper that it tried to perform
 * an illegal mapping and allow it to retry.
 *
 * To introduce how to write a custom mapper
 * we'll implement an adversarial mapper 
 * that makes random mapping decisions
 * designed to stress the Legion runtime. 
 * We'll report the chosen mapping decisions
 * to show that Legion computes the correct
 * answer regardless of the mapping.
 */

// Mappers are classes that implement the
// mapping interface declared in legion.h.
// Legion provides a default impelmentation
// of this interface defined by the
// DefaultMapper class.  Programmers can
// implement custom mappers either by 
// extending the DefaultMapper class
// or by declaring their own class which
// implements the mapping interface from
// scratch.  Here we will extend the
// DefaultMapper which means that we only
// need to override the methods that we
// want to in order to change our mapping.
// In this example, we'll override four
// of the mapping calls to illustrate
// how they work.
class AdversarialMapper : public DefaultMapper {
public:
  AdversarialMapper(Machine *machine, 
      HighLevelRuntime *rt, Processor local);
public:
  virtual void select_task_options(Task *task);
  virtual void slice_domain(const Task *task, const Domain &domain,
                            std::vector<DomainSplit> &slices);
  virtual bool map_task(Task *task); 
  virtual void notify_mapping_result(const Mappable *mappable);
};

class PartitioningMapper : public DefaultMapper {
public:
  PartitioningMapper(Machine *machine,
      HighLevelRuntime *rt, Processor local);
public:
  virtual int get_tunable_value(const Task *task,
                                TunableID tid,
                                MappingTagID tag);
};

// Mappers are created after the Legion runtime
// starts but before the application begins 
// executing.  To create mappers the application
// registers a callback function for the runtime
// to perform prior to beginning execution.  In
// this example we call it the 'mapper_registration'
// function.  (See below for where we register
// this callback with the runtime.)  The callback
// function must have this type, which allows the
// runtime to pass the necessary paramterers in
// for creating new mappers.
//
// In Legion, mappers are identified by a MapperID.
// Zero is reserved for the DefaultMapper, but 
// other mappers can replace it by using the 
// 'replace_default_mapper' call.  Other mappers
// can be registered with their own IDs using the
// 'add_mapper' method call on the runtime.
//
// The model for Legion is that there should always
// be one mapper for every processor in the system.
// This guarantees that there is never contention
// for the mappers because multiple processors need
// to access the same mapper object.  When the
// runtime invokes the 'mapper_registration' callback,
// it gives a list of local processors which 
// require a mapper if a new mapper class is to be
// created.  In a multi-node setting, the runtime
// passes in a subset of the processors for which
// mappers need to be created on the local node.
//
// Here we override the DefaultMapper ID so that
// all tasks that normally would have used the
// DefaultMapper will now use our AdversarialMapper.
// We create one new mapper for each processor
// and register it with the runtime.
void mapper_registration(Machine *machine, HighLevelRuntime *rt,
                          const std::set<Processor> &local_procs)
{
  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    rt->replace_default_mapper(
        new AdversarialMapper(machine, rt, *it), *it);
    rt->add_mapper(PARTITIONING_MAPPER_ID,
        new PartitioningMapper(machine, rt, *it), *it);
  }
}

// Here is the constructor for our adversial mapper.
// We'll use the constructor to illustrate how mappers can
// get access to information regarding the current machine.
AdversarialMapper::AdversarialMapper(Machine *m, 
                                     HighLevelRuntime *rt, Processor p)
  : DefaultMapper(m, rt, p) // pass arguments through to DefaultMapper
{
  // The machine object is a singleton object that can be
  // used to get information about the underlying hardware.
  // The machine pointer will always be provided to all
  // mappers, but can be accessed anywhere by the static
  // member method Machine::get_machine().  Here we get
  // a reference to the set of all processors in the machine
  // from the machine object.  Note that the Machine object
  // actually comes from the Legion low-level runtime, most
  // of which is not directly needed by application code.
  // Typedefs in legion_types.h ensure that all necessary
  // types for querying the machine object are in scope
  // in the Legion HighLevel namespace.
  const std::set<Processor> &all_procs = machine->get_all_processors();
  // Recall that we create one mapper for every processor.  We
  // only want to print out this information one time, so only
  // do it if we are the mapper for the first processor in the
  // list of all processors in the machine.
  if ((*(all_procs.begin())) == local_proc)
  {
    // Print out how many processors there are and each
    // of their kinds.
    printk("There are %ld processors:\n", all_procs.size());
    for (std::set<Processor>::const_iterator it = all_procs.begin();
          it != all_procs.end(); it++)
    {
      // For every processor there is an associated kind
      Processor::Kind kind = machine->get_processor_kind(*it);
      switch (kind)
      {
        // Latency-optimized cores (LOCs) are CPUs
        case Processor::LOC_PROC:
          {
            printk("  Processor ID %x is CPU\n", it->id); 
            break;
          }
        // Throughput-optimized cores (TOCs) are GPUs
        case Processor::TOC_PROC:
          {
            printk("  Processor ID %x is GPU\n", it->id);
            break;
          }
        // Utility processors are helper processors for
        // running Legion runtime meta-level tasks and 
        // should not be used for running application tasks
        case Processor::UTIL_PROC:
          {
            printk("  Processor ID %x is utility\n", it->id);
            break;
          }
        default:
          assert(false);
      }
    }
    // We can also get the list of all the memories available
    // on the target architecture and print out their info.
    const std::set<Memory> &all_mems = machine->get_all_memories();
    printk("There are %ld memories:\n", all_mems.size());
    for (std::set<Memory>::const_iterator it = all_mems.begin();
          it != all_mems.end(); it++)
    {
      Memory::Kind kind = machine->get_memory_kind(*it);
      size_t memory_size_in_kb = machine->get_memory_size(*it) >> 10;
      switch (kind)
      {
        // RDMA addressable memory when running with GASNet
        case Memory::GLOBAL_MEM:
          {
            printk("  GASNet Global Memory ID %x has %ld KB\n", 
                    it->id, memory_size_in_kb);
            break;
          }
        // DRAM on a single node
        case Memory::SYSTEM_MEM:
          {
            printk("  System Memory ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // Pinned memory on a single node
        case Memory::REGDMA_MEM:
          {
            printk("  Pinned Memory ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // A memory associated with a single socket
        case Memory::SOCKET_MEM:
          {
            printk("  Socket Memory ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // Zero-copy memory betweeen CPU DRAM and
        // all GPUs on a single node
        case Memory::Z_COPY_MEM:
          {
            printk("  Zero-Copy Memory ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // GPU framebuffer memory for a single GPU
        case Memory::GPU_FB_MEM:
          {
            printk("  GPU Frame Buffer Memory ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // Block of memory sized for L3 cache
        case Memory::LEVEL3_CACHE:
          {
            printk("  Level 3 Cache ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // Block of memory sized for L2 cache
        case Memory::LEVEL2_CACHE:
          {
            printk("  Level 2 Cache ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        // Block of memory sized for L1 cache
        case Memory::LEVEL1_CACHE:
          {
            printk("  Level 1 Cache ID %x has %ld KB\n",
                    it->id, memory_size_in_kb);
            break;
          }
        default:
          assert(false);
      }
    }

    // The Legion machine model represented by the machine object
    // can be thought of as a graph with processors and memories
    // as the two kinds of nodes.  There are two kinds of edges
    // in this graph: processor-memory edges and memory-memory
    // edges.  An edge between a processor and a memory indicates
    // that the processor can directly perform load and store
    // operations to that memory.  Memory-memory edges indicate
    // that data movement can be directly performed between the
    // two memories.  To illustrate how this works we examine
    // all the memories visible to our local processor in 
    // this mapper.  We can get our set of visible memories
    // using the 'get_visible_memories' method on the machine.
    const std::set<Memory> vis_mems = machine->get_visible_memories(local_proc);
    printk("There are %ld memories visible from processor %x\n",
            vis_mems.size(), local_proc.id);
    for (std::set<Memory>::const_iterator it = vis_mems.begin();
          it != vis_mems.end(); it++)
    {
      // Edges between nodes are called affinities in the
      // machine model.  Affinities also come with approximate
      // indications of the latency and bandwidth between the 
      // two nodes.  Right now these are unit-less measurements,
      // but our plan is to teach the Legion runtime to profile
      // these values on start-up to give them real values
      // and further increase the portability of Legion applications.
      std::vector<ProcessorMemoryAffinity> affinities;
      int results = 
        machine->get_proc_mem_affinity(affinities, local_proc, *it);
      // We should only have found 1 results since we
      // explicitly specified both values.
      assert(results == 1);
      printk("  Memory %x has bandwidth %d and latency %d\n",
              it->id, affinities[0].bandwidth, affinities[0].latency);
    }
  }
}

// The first mapper call that we override is the 
// select_task_options call.  This mapper call is
// performed on every task launch immediately after
// it is made.  The call asks the mapper to select 
// set properities of the task:
//
//  inline_task - whether the task should be directly
//    inlined into its parent task, using the parent
//    task's physical regions.  
//  spawn_task - whether the task is eligible for 
//    stealing (based on Cilk-style semantics)
//  map_locally - whether the task should be mapped
//    by the processor on which it was launched or
//    whether it should be mapped by the processor
//    where it will run.
//  profile_task - should the runtime collect profiling
//    information about the task while it is executing
//  target_proc - which processor should the task be
//    sent to once all of its mapping dependences have
//    been satisifed.
//
//  Note that these properties are all set on the Task
//  object declared in legion.h.  The Task object is
//  the representation of a task that the Legion runtime
//  provides to the mapper for specifying mapping
//  decisions.  Note that there are similar objects
//  for inline mappings as well as other operations.
//
//  For our adversarial mapper, we perform the default
//  choices for all options except the last one.  Here
//  we choose a random processor in our system to 
//  send the task to.
void AdversarialMapper::select_task_options(Task *task)
{
  task->inline_task = false;
  task->spawn_task = false;
  task->map_locally = false;
  task->profile_task = false;
  task->task_priority = 0;
  const std::set<Processor> &all_procs = machine->get_all_processors();
  task->target_proc = 
    DefaultMapper::select_random_processor(all_procs, Processor::LOC_PROC, machine);
}

// The second call that we override is the slice_domain
// method. The slice_domain call is used by the runtime
// to query the mapper about the best way to distribute
// the points in an index space task launch throughout
// the machine. The maper is given the task and the domain
// to slice and then asked to generate sub-domains to be
// sent to different processors in the form of DomainSplit
// objects. DomainSplit objects describe the sub-domain,
// the target processor for the sub-domain, whether the
// generated slice can be stolen, and finally whether 
// slice_domain' should be recursively called on the
// slice when it arrives at its destination.
//
// In this example we use a utility method from the DefaultMapper
// called decompose_index_space to decompose our domain. We 
// recursively split the domain in half during each call of
// slice_domain and send each slice to a random processor.
// We continue doing this until the leaf domains have only
// a single point in them. This creates a tree of slices of
// depth log(N) in the number of points in the domain with
// each slice being sent to a random processor.
void AdversarialMapper::slice_domain(const Task *task, const Domain &domain,
                                     std::vector<DomainSplit> &slices)
{
  const std::set<Processor> &all_procs = machine->get_all_processors();
  std::vector<Processor> split_set;
  for (unsigned idx = 0; idx < 2; idx++)
  {
    split_set.push_back(DefaultMapper::select_random_processor(
                        all_procs, Processor::LOC_PROC, machine));
  }

  DefaultMapper::decompose_index_space(domain, split_set, 
                                        1/*splitting factor*/, slices);
  for (std::vector<DomainSplit>::iterator it = slices.begin();
        it != slices.end(); it++)
  {
    Rect<1> rect = it->domain.get_rect<1>();
    if (rect.volume() == 1)
      it->recurse = false;
    else
      it->recurse = true;
  }
}

// The next mapping call that we override is the map_task
// mapper method. Once a task has been assigned to map on
// a specific processor (the target_proc) then this method
// is invoked by the runtime to select the memories in 
// which to create physical instances for each logical region.
// The mapper communicates this information to the runtime
// via the mapping fields on RegionRequirements. The memories
// containing currently valid physical instances for each
// logical region is provided by the runtime in the 
// 'current_instances' field. The mapper must specify an
// ordered list of memories for the runtime to try when
// creating a physical instance in the 'target_ranking'
// vector field of each RegionRequirement. The runtime
// attempts to find or make a physical instance in each 
// memory until it succeeds. If the runtime fails to find
// or make a physical instance in any of the memories, then
// the mapping fails and the mapper will be notified that
// the task failed to map using the 'notify_mapping_failed'
// mapper call. If the mapper does nothing, then the task
// is placed back on the list of tasks eligible to be mapped.
// There are other fields that the mapper can set in the
// process of the map_task call that we do not cover here.
//
// In this example, the mapper finds the set of all visible
// memories from the target processor and then puts them
// in a random order as the target set of memories, thereby
// challenging the Legion runtime to maintain correctness
// of data moved through random sets of memories.
bool AdversarialMapper::map_task(Task *task)
{ 
  const std::set<Memory> &vis_mems = 
      machine->get_visible_memories(task->target_proc);  
  assert(!vis_mems.empty());
  for (unsigned idx = 0; idx < task->regions.size(); idx++)
  {
    std::set<Memory> mems_copy = vis_mems;  
    // Assign memories in a random order
    while (!mems_copy.empty())
    {
      unsigned mem_idx = (lrand48() % mems_copy.size());
      std::set<Memory>::iterator it = mems_copy.begin();
      for (unsigned i = 0; i < mem_idx; i++)
        it++;
      task->regions[idx].target_ranking.push_back(*it);
      mems_copy.erase(it);
    }
    task->regions[idx].virtual_map = false;
    task->regions[idx].enable_WAR_optimization = false;
    task->regions[idx].reduction_list = false;
    task->regions[idx].blocking_factor = 1;
  }
  // Report successful mapping results
  return true;
}

// The last mapper call we override is the notify_mapping_result
// call which is invoked by the runtime if the mapper indicated
// that it wanted to know the result of the mapping following
// the map_task call by returning true. The runtime invokes
// this call and the chosen memories for each RegionRequirement
// are set in the 'selected_memory' field. We use this call in
// this example to record the memories in which physical instances
// were mapped for each logical region of each task so we can
// see that the assignment truly is random.
void AdversarialMapper::notify_mapping_result(const Mappable *mappable)
{
  if (mappable->get_mappable_kind() == Mappable::TASK_MAPPABLE)
  {
    const Task *task = mappable->as_mappable_task();
    assert(task != NULL);
    for (unsigned idx = 0; idx < task->regions.size(); idx++)
    {
      printk("Mapped region %d of task %s (ID %lld) to memory %x\n",
              idx, task->variants->name, 
              task->get_unique_task_id(),
              task->regions[idx].selected_memory.id);
    }
  }
}

PartitioningMapper::PartitioningMapper(Machine *m,
                                       HighLevelRuntime *rt,
                                       Processor p)
  : DefaultMapper(m, rt, p)
{
}

int PartitioningMapper::get_tunable_value(const Task *task,
                                          TunableID tid,
                                          MappingTagID tag)
{
  if (tid == SUBREGION_TUNABLE)
  {
    const std::set<Processor> &cpu_procs = 
      machine_interface.filter_processors(Processor::LOC_PROC);
    return cpu_procs.size();
  }
  // Should never get here
  assert(false);
  return 0;
}

static inline unsigned long long 
rdtsc (void)
{
    unsigned lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo | ((unsigned long long)(hi) << 32);
}
/*
 * Everything below here is the standard daxpy example
 * except for the registration of the callback function
 * for creating custom mappers which is explicitly commented
 * and the call to get_tunable_value to determine the number
 * of sub-regions.
 */
void top_level_task(const Task *task,
                    const std::vector<PhysicalRegion> &regions,
                    Context ctx, HighLevelRuntime *runtime)
{
  int num_elements = 1024; 
  {
    const InputArgs &command_args = HighLevelRuntime::get_input_args();
    for (int i = 1; i < command_args.argc; i++)
    {
      if (!strcmp(command_args.argv[i],"-n"))
        num_elements = atoi(command_args.argv[++i]);
    }
  }
  int num_subregions = runtime->get_tunable_value(ctx, SUBREGION_TUNABLE, 
                                                  PARTITIONING_MAPPER_ID);

  printk("Running daxpy for %d elements...\n", num_elements);
  printk("Partitioning data into %d sub-regions...\n", num_subregions);

  Rect<1> elem_rect(Point<1>(0),Point<1>(num_elements-1));
  IndexSpace is = runtime->create_index_space(ctx, 
                          Domain::from_rect<1>(elem_rect));
  FieldSpace input_fs = runtime->create_field_space(ctx);
  {
    FieldAllocator allocator = 
      runtime->create_field_allocator(ctx, input_fs);
    allocator.allocate_field(sizeof(double),FID_X);
    allocator.allocate_field(sizeof(double),FID_Y);
  }
  FieldSpace output_fs = runtime->create_field_space(ctx);
  {
    FieldAllocator allocator = 
      runtime->create_field_allocator(ctx, output_fs);
    allocator.allocate_field(sizeof(double),FID_Z);
  }
  LogicalRegion input_lr = runtime->create_logical_region(ctx, is, input_fs);
  LogicalRegion output_lr = runtime->create_logical_region(ctx, is, output_fs);

  Rect<1> color_bounds(Point<1>(0),Point<1>(num_subregions-1));
  Domain color_domain = Domain::from_rect<1>(color_bounds);

  IndexPartition ip;
  if ((num_elements % num_subregions) != 0)
  {
    const int lower_bound = num_elements/num_subregions;
    const int upper_bound = lower_bound+1;
    const int number_small = num_subregions - (num_elements % num_subregions);
    DomainColoring coloring;
    int index = 0;
    for (int color = 0; color < num_subregions; color++)
    {
      int num_elmts = color < number_small ? lower_bound : upper_bound;
      assert((index+num_elmts) <= num_elements);
      Rect<1> subrect(Point<1>(index),Point<1>(index+num_elmts-1));
      coloring[color] = Domain::from_rect<1>(subrect);
      index += num_elmts;
    }
    ip = runtime->create_index_partition(ctx, is, color_domain, 
                                      coloring, true/*disjoint*/);
  }
  else
  { 
    Blockify<1> coloring(num_elements/num_subregions);
    ip = runtime->create_index_partition(ctx, is, coloring);
  }

  LogicalPartition input_lp = runtime->get_logical_partition(ctx, input_lr, ip);
  LogicalPartition output_lp = runtime->get_logical_partition(ctx, output_lr, ip);

  Domain launch_domain = color_domain; 
  ArgumentMap arg_map;

  IndexLauncher init_launcher(INIT_FIELD_TASK_ID, launch_domain, 
                              TaskArgument(NULL, 0), arg_map);
  init_launcher.add_region_requirement(
      RegionRequirement(input_lp, 0/*projection ID*/, 
                        WRITE_DISCARD, EXCLUSIVE, input_lr));
  init_launcher.add_field(0, FID_X);
  runtime->execute_index_space(ctx, init_launcher);

  init_launcher.region_requirements[0].privilege_fields.clear();
  init_launcher.region_requirements[0].instance_fields.clear();
  init_launcher.add_field(0, FID_Y);
  runtime->execute_index_space(ctx, init_launcher);

  //const double alpha = drand48();
  const double alpha = rdtsc()/100.0;
  IndexLauncher daxpy_launcher(DAXPY_TASK_ID, launch_domain,
                TaskArgument(&alpha, sizeof(alpha)), arg_map);
  daxpy_launcher.add_region_requirement(
      RegionRequirement(input_lp, 0/*projection ID*/,
                        READ_ONLY, EXCLUSIVE, input_lr));
  daxpy_launcher.add_field(0, FID_X);
  daxpy_launcher.add_field(0, FID_Y);
  daxpy_launcher.add_region_requirement(
      RegionRequirement(output_lp, 0/*projection ID*/,
                        WRITE_DISCARD, EXCLUSIVE, output_lr));
  daxpy_launcher.add_field(1, FID_Z);
  runtime->execute_index_space(ctx, daxpy_launcher);
                    
  TaskLauncher check_launcher(CHECK_TASK_ID, TaskArgument(&alpha, sizeof(alpha)));
  check_launcher.add_region_requirement(
      RegionRequirement(input_lr, READ_ONLY, EXCLUSIVE, input_lr));
  check_launcher.region_requirements[0].add_field(FID_X);
  check_launcher.region_requirements[0].add_field(FID_Y);
  check_launcher.add_region_requirement(
      RegionRequirement(output_lr, READ_ONLY, EXCLUSIVE, output_lr));
  check_launcher.region_requirements[1].add_field(FID_Z);
  runtime->execute_task(ctx, check_launcher);

  runtime->destroy_logical_region(ctx, input_lr);
  runtime->destroy_logical_region(ctx, output_lr);
  runtime->destroy_field_space(ctx, input_fs);
  runtime->destroy_field_space(ctx, output_fs);
  runtime->destroy_index_space(ctx, is);
}

void init_field_task(const Task *task,
                     const std::vector<PhysicalRegion> &regions,
                     Context ctx, HighLevelRuntime *runtime)
{
  assert(regions.size() == 1); 
  assert(task->regions.size() == 1);
  assert(task->regions[0].privilege_fields.size() == 1);

  FieldID fid = *(task->regions[0].privilege_fields.begin());
  const int point = task->index_point.point_data[0];
  printk("Initializing field %d for block %d...\n", fid, point);

  RegionAccessor<AccessorType::Generic, double> acc = 
    regions[0].get_field_accessor(fid).typeify<double>();

  Domain dom = runtime->get_index_space_domain(ctx, 
      task->regions[0].region.get_index_space());
  Rect<1> rect = dom.get_rect<1>();
  for (GenericPointInRectIterator<1> pir(rect); pir; pir++)
  {
    //acc.write(DomainPoint::from_point<1>(pir.p), drand48());
    acc.write(DomainPoint::from_point<1>(pir.p), (double)rdtsc());
  }
}

void daxpy_task(const Task *task,
                const std::vector<PhysicalRegion> &regions,
                Context ctx, HighLevelRuntime *runtime)
{
  assert(regions.size() == 2);
  assert(task->regions.size() == 2);
  assert(task->arglen == sizeof(double));
  const double alpha = *((const double*)task->args);
  const int point = task->index_point.point_data[0];

  RegionAccessor<AccessorType::Generic, double> acc_x = 
    regions[0].get_field_accessor(FID_X).typeify<double>();
  RegionAccessor<AccessorType::Generic, double> acc_y = 
    regions[0].get_field_accessor(FID_Y).typeify<double>();
  RegionAccessor<AccessorType::Generic, double> acc_z = 
    regions[1].get_field_accessor(FID_Z).typeify<double>();
  printk("Running daxpy computation with alpha %.8g for point %d...\n", 
          alpha, point);

  Domain dom = runtime->get_index_space_domain(ctx, 
      task->regions[0].region.get_index_space());
  Rect<1> rect = dom.get_rect<1>();
  for (GenericPointInRectIterator<1> pir(rect); pir; pir++)
  {
    double value = alpha * acc_x.read(DomainPoint::from_point<1>(pir.p)) + 
                           acc_y.read(DomainPoint::from_point<1>(pir.p));
    acc_z.write(DomainPoint::from_point<1>(pir.p), value);
  }
}

void check_task(const Task *task,
                const std::vector<PhysicalRegion> &regions,
                Context ctx, HighLevelRuntime *runtime)
{
  assert(regions.size() == 2);
  assert(task->regions.size() == 2);
  assert(task->arglen == sizeof(double));
  const double alpha = *((const double*)task->args);
  RegionAccessor<AccessorType::Generic, double> acc_x = 
    regions[0].get_field_accessor(FID_X).typeify<double>();
  RegionAccessor<AccessorType::Generic, double> acc_y = 
    regions[0].get_field_accessor(FID_Y).typeify<double>();
  RegionAccessor<AccessorType::Generic, double> acc_z = 
    regions[1].get_field_accessor(FID_Z).typeify<double>();
  printk("Checking results...");
  Domain dom = runtime->get_index_space_domain(ctx, 
      task->regions[0].region.get_index_space());
  Rect<1> rect = dom.get_rect<1>();
  bool all_passed = true;
  for (GenericPointInRectIterator<1> pir(rect); pir; pir++)
  {
    double expected = alpha * acc_x.read(DomainPoint::from_point<1>(pir.p)) + 
                           acc_y.read(DomainPoint::from_point<1>(pir.p));
    double received = acc_z.read(DomainPoint::from_point<1>(pir.p));
    // Probably shouldn't check for floating point equivalence but
    // the order of operations are the same should they should
    // be bitwise equal.
    if (expected != received)
      all_passed = false;
  }
  if (all_passed)
    printk("SUCCESS!\n");
  else
    printk("FAILURE!\n");
}

int go_custom(int argc, char **argv);
int go_custom(int argc, char **argv)
{
  HighLevelRuntime::set_top_level_task_id(TOP_LEVEL_TASK_ID);
  HighLevelRuntime::register_legion_task<top_level_task>(TOP_LEVEL_TASK_ID,
      Processor::LOC_PROC, true/*single*/, false/*index*/);
  HighLevelRuntime::register_legion_task<init_field_task>(INIT_FIELD_TASK_ID,
      Processor::LOC_PROC, true/*single*/, true/*index*/);
  HighLevelRuntime::register_legion_task<daxpy_task>(DAXPY_TASK_ID,
      Processor::LOC_PROC, true/*single*/, true/*index*/);
  HighLevelRuntime::register_legion_task<check_task>(CHECK_TASK_ID,
      Processor::LOC_PROC, true/*single*/, true/*index*/);

  // Here is where we register the callback function for 
  // creating custom mappers.
  HighLevelRuntime::set_registration_callback(mapper_registration);

  return HighLevelRuntime::start(argc, argv);
}

extern "C" void go_custom_c (int argc, char ** argv) {
    go_custom(argc, argv);
}

