/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cassert>
#include <numeric>
#include <string>
#include <cmath>

#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>

#include "model/buffer.hpp"
//BOOST_CLASS_EXPORT(model::BufferLevel::Specs)
BOOST_CLASS_EXPORT(model::BufferLevel)

#include "util/numeric.hpp"
#include "util/misc.hpp"
#include "pat/pat.hpp"
#include "topology.hpp"

namespace model
{

// ==================================== //
//             Buffer Level             //
// ==================================== //

BufferLevel::BufferLevel()
{ }

BufferLevel::BufferLevel(const Specs& specs) :
    specs_(specs)
{
  is_specced_ = true;
  is_evaluated_ = false;
}

BufferLevel::~BufferLevel()
{ }

// The hierarchical ParseSpecs functions are static and do not
// affect the internal specs_ data structure, which is set by
// the dynamic Spec() call later.
BufferLevel::Specs BufferLevel::ParseSpecs(config::CompoundConfigNode level, uint32_t n_elements)
{
  auto& buffer = level;

  Specs specs;

  // Name. This has to go first. Since the rest can be attributes
  std::string name;
  if (buffer.lookupValue("name", name))
  {
    specs.name = config::parseName(name);
  }

  std::string className = "";
  if (buffer.exists("attributes"))
  {
    buffer.lookupValue("class", className);
    buffer = buffer.lookup("attributes");
  }

  // Word Bits.
  std::uint32_t word_bits;
  if (buffer.lookupValue("word-bits", word_bits) ||
      buffer.lookupValue("word_width", word_bits) ||
      buffer.lookupValue("datawidth", word_bits) )
  {
    specs.word_bits = word_bits;
  }
  else
  {
    specs.word_bits = Specs::kDefaultWordBits;
  }

  // Block size.
  std::uint32_t block_size;
  specs.block_size = 1;
  if (buffer.lookupValue("block-size", block_size) ||
      buffer.lookupValue("n_words", block_size) )
  {
    specs.block_size = block_size;
  }

  // MetaData Block size
  std::uint32_t metadata_block_size;
  specs.metadata_block_size = 1;
  if (buffer.lookupValue("metadata-block-size", metadata_block_size))
  {
    specs.metadata_block_size = block_size;
  }

  // Metadata data width
  // we currently consider metadata to be stored in the same storage
  // metadata data width is important to get a realistic size for the metadata
  // default to 0 -> no metadata
  // FIXME: consider metatdata as its own dataspace
  std::uint32_t metadata_word_bits;
  specs.metadata_word_bits = 0;
  if (buffer.lookupValue("metadata_datawidth", metadata_word_bits)){
     specs.metadata_word_bits = metadata_word_bits;
  }

  // Cluster size.
  std::uint32_t cluster_size;
  specs.cluster_size = 1;
  std::uint32_t width;
  if (buffer.lookupValue("cluster-size", cluster_size))
  {
    specs.cluster_size = cluster_size;
  }
  else if (buffer.lookupValue("width", width)||
           buffer.lookupValue("memory_width", width))
  {
    word_bits = specs.word_bits.Get();
    block_size = specs.block_size.Get();
    if (width % (word_bits * block_size)  != 0){
      std::cout << "ERROR: width: " << width << "  block_size: " << block_size << "  word_bits: " << word_bits << std::endl;
    }
    assert(width % (word_bits * block_size)  == 0);
    specs.cluster_size = width / (word_bits * block_size);
  }

  // Size.
  // It has dependency on BlockSize and thus is initialized after BlockSize.
  std::uint32_t size;
  if (buffer.lookupValue("entries", size) )
  {
    assert(buffer.exists("sizeKB") == false);
    specs.size = size;
  }
  else if (buffer.lookupValue("depth", size) ||
           buffer.lookupValue("memory_depth", size))
  {
    assert(buffer.exists("sizeKB") == false);
    assert(buffer.exists("entries") == false);
    specs.size = size * specs.block_size.Get();
  }
  else if (buffer.lookupValue("sizeKB", size))
  {
    specs.size = size * 1024 * 8 / specs.word_bits.Get();
  }


  // Technology.
  // Unfortunately ".technology" means different things between ISPASS format
  // and Accelergy v0.2 format. So we use the class name to find out what to
  // assume.
  std::string technology;
  specs.technology = Technology::SRAM;
  if (className == "DRAM") specs.technology = Technology::DRAM;
  if (className.find("DRAM") != std::string::npos) specs.technology = Technology::DRAM;

  if (buffer.lookupValue("technology", technology) && technology == "DRAM")
  {
    specs.technology = Technology::DRAM;
  }

  // SRAM Type.
  std::uint32_t num_ports = 2;
  specs.num_ports = num_ports;
  if (buffer.lookupValue("num-ports", num_ports))
  {
    if (num_ports == 1)
    {
      specs.num_ports = num_ports;
    }
    else
    {
      assert(num_ports == 2);
    }
  }

  // Number of Banks.
  std::uint32_t num_banks = 2;
  specs.num_banks = num_banks;
  if (buffer.lookupValue("num-banks", num_banks))
  {
    specs.num_banks = num_banks;
  }

  // Bandwidth.
  double bandwidth;
  if (buffer.lookupValue("bandwidth", bandwidth))
  {
    std::cerr << "WARNING: bandwidth is deprecated. Assuming read_bandwidth = write_bandwidth = bandwidth/2" << std::endl;
    specs.read_bandwidth  = bandwidth / 2;
    specs.write_bandwidth = bandwidth / 2;
  }

  double read_bandwidth;
  if (buffer.lookupValue("read_bandwidth", read_bandwidth))
  {
    specs.read_bandwidth = read_bandwidth;
  }

  double write_bandwidth;
  if (buffer.lookupValue("write_bandwidth", write_bandwidth))
  {
    specs.write_bandwidth = write_bandwidth;
  }

  // Multiple-buffering factor (e.g., 2.0 means double buffering)
  double multiple_buffering;
  if (buffer.lookupValue("multiple-buffering", multiple_buffering))
  {
    specs.multiple_buffering = multiple_buffering;
  }
  else
  {
    specs.multiple_buffering = 1.0;
  }
  
  if (specs.size.IsSpecified())
  {
    specs.effective_size = static_cast<uint64_t>(std::floor(
            specs.size.Get() / specs.multiple_buffering.Get()));
  }

  // Minimum utilization factor (e.g., 1.0 requires full utilization of effective capacity)
  double min_utilizaiton;
  if (buffer.lookupValue("min-utilization", min_utilizaiton))
  {
    specs.min_utilization = min_utilizaiton;
  }
  else
  {
    specs.min_utilization = 0.0;
  }
  if (specs.min_utilization.Get() != 0.0)
  {
    assert(specs.effective_size.IsSpecified());
  }

  // Instances.
  std::uint32_t instances;
  if (buffer.lookupValue("instances", instances))
  {
    specs.instances = instances;
  } else {
    specs.instances = n_elements;
  }

  // MeshX.
  std::uint32_t meshX;
  if (buffer.lookupValue("meshX", meshX))
  {
    specs.meshX = meshX;
  }

  // MeshY.
  std::uint32_t meshY;
  if (buffer.lookupValue("meshY", meshY))
  {
    specs.meshY = meshY;
  }

  // Network names;
  std::string read_network_name;
  if (buffer.lookupValue("network_read", read_network_name))
  {
    specs.read_network_name = read_network_name;
  }

  std::string fill_network_name;
  if (buffer.lookupValue("network_fill", fill_network_name))
  {
    specs.fill_network_name = fill_network_name;
  }

  std::string drain_network_name;
  if (buffer.lookupValue("network_drain", drain_network_name))
  {
    specs.drain_network_name = drain_network_name;
  }

  std::string update_network_name;
  if (buffer.lookupValue("network_update", update_network_name))
  {
    specs.update_network_name = update_network_name;
  }

  // Vector Access Energy
  double tmp_access_energy = 0;
  double tmp_storage_area = 0;

  if (specs.technology.Get() == Technology::DRAM)
  {
    assert(specs.cluster_size.Get() == 1);
    tmp_access_energy = pat::DRAMEnergy(specs.word_bits.Get() * specs.block_size.Get());
    tmp_storage_area = 0;
  }
  else if (specs.size.Get() == 0)
  {
    //SRAM
    tmp_access_energy = 0;
    tmp_storage_area = 0;
  }
  else
  {
    std::uint64_t tmp_entries = specs.size.Get();
    std::uint64_t tmp_word_bits = specs.word_bits.Get();
    std::uint64_t tmp_block_size = specs.block_size.Get();
    std::uint64_t tmp_cluster_size = specs.cluster_size.Get();
    std::uint64_t width = tmp_word_bits * tmp_block_size * tmp_cluster_size;
    std::uint64_t height =
      (tmp_entries % tmp_block_size == 0) ?
      (tmp_entries / tmp_block_size)      :
      (tmp_entries / tmp_block_size) + 1;  
    tmp_access_energy = pat::SRAMEnergy(height, width, specs.num_banks.Get(), specs.num_ports.Get()) / tmp_cluster_size;
    tmp_storage_area = pat::SRAMArea(height, width, specs.num_banks.Get(), specs.num_ports.Get()) / tmp_cluster_size;
    // std::cout << "Entries = " << tmp_entries
    //           << ", word_size = " << tmp_word_bits
    //           << ", block_size = " << tmp_block_size
    //           << ", cluster_size = " << tmp_cluster_size
    //           << ", num_banks = " << specs.num_banks.Get()
    //           << ", num_ports = " << specs.num_ports.Get()
    //           << ", energy = " << tmp_access_energy
    //           << ", area = " << tmp_storage_area << std::endl;
  }

  // Allow user to override the access energy.
  buffer.lookupValue("vector-access-energy", tmp_access_energy);

  // Allow user to override the addr gen energy.
  double tmp_addr_gen_energy = -0.1;
  buffer.lookupValue("addr-gen-energy", tmp_addr_gen_energy);
  specs.addr_gen_energy = tmp_addr_gen_energy;

  // Allow user to override the cluster area.
  double tmp_cluster_area = 0;
  buffer.lookupValue("cluster-area", tmp_cluster_area);
  if (tmp_cluster_area > 0)
    tmp_storage_area = tmp_cluster_area / specs.cluster_size.Get();

  // Set final physical dimensions and energy.
  specs.vector_access_energy = tmp_access_energy;
  specs.storage_area = tmp_storage_area; //FIXME: check with Angshu

  // std::cout << "BUFFER " << specs.name << " vector access energy = "
  //           << specs.vector_access_energy << " pJ, cluster area = "
  //           << specs.storage_area.Get() * specs.cluster_size.Get()
  //           << " um^2" << std::endl;

  specs.level_name = specs.name.Get();

  ValidateTopology(specs);
    
  return specs;
}

// Make sure the topology is consistent,
// and update unspecified parameters if they can
// be inferred from other specified parameters.
void BufferLevel::ValidateTopology(BufferLevel::Specs& specs)
{
  bool error = false;
  if (specs.instances.IsSpecified())
  {
    if (specs.meshX.IsSpecified())
    {
      if (specs.meshY.IsSpecified())
      {
        // All 3 are specified.
        assert(specs.meshX.Get() * specs.meshY.Get() == specs.instances.Get());
      }
      else
      {
        // Instances and MeshX are specified.
        assert(specs.instances.Get() % specs.meshX.Get() == 0);
        specs.meshY = specs.instances.Get() / specs.meshX.Get();
      }
    }
    else if (specs.meshY.IsSpecified())
    {
      // Instances and MeshY are specified.
      assert(specs.instances.Get() % specs.meshY.Get() == 0);
      specs.meshX = specs.instances.Get() / specs.meshY.Get();
    }
    else
    {
      // Only Instances is specified.
      specs.meshX = specs.instances.Get();
      specs.meshY = 1;
    }
  }
  else if (specs.meshX.IsSpecified())
  {
    if (specs.meshY.IsSpecified())
    {
      // MeshX and MeshY are specified.
      specs.instances = specs.meshX.Get() * specs.meshY.Get();
    }
    else
    {
      // Only MeshX is specified. We can make assumptions but it's too dangerous.
      error = true;
    }
  }
  else if (specs.meshY.IsSpecified())
  {
    // Only MeshY is specified. We can make assumptions but it's too dangerous.
    error = true;
  }
  else
  {
    // Nothing is specified.
    error = true;
  }

  if (error)
  {
    std::cerr << "ERROR: " << specs.name.Get()
              << ": instances and/or meshX * meshY must be specified."
              << std::endl;
    exit(1);        
  }
}


void BufferLevel::PopulateEnergyPerOp(unsigned num_ops){

  if (! populate_energy_per_op){

    double ert_energy_per_op;
    bool  ert_energy_found;
    std::vector<std::string> ert_action_names;
    std::string op_name;




    for (unsigned op_id = 0; op_id < num_ops; op_id++){
      // go through all op types
      ert_energy_per_op = 0;
      ert_energy_found = false;
      op_name = tiling::storageOperationTypes[op_id];

     // initialize to the pat values or zero in case no mapping is found
      if (op_name.find("random_read") != std::string::npos
          || op_name.find("random_fill") != std::string::npos
          || op_name.find("random_update") != std::string::npos){
            // use the max if no mapping is found for regular memory actions
            ert_energy_per_op = specs_.vector_access_energy.Get();
      } else {
          // use zero if no mapping is found for matadata/gated/skipped/decompression/compression actions
          ert_energy_per_op = 0;
      }

      // go through ERT entries and look for appopriate energy values
      // std::cout <<"operation name: " << op_name << std::endl;
      ert_action_names = model::storageOperationMappings.at(op_name);
      for (auto it = ert_action_names.begin(); it != ert_action_names.end(); it++){
        if(specs_.ERT_entries.count(*it)>0 && (!ert_energy_found)){
          ert_energy_per_op = specs_.ERT_entries.at(*it);
          ert_energy_found = true;
        }
      }
      // populate the op_energy_map data structure for easier future energy search
      specs_.op_energy_map[op_name] = ert_energy_per_op;
  }
  populate_energy_per_op = true;

 }

}


// PreEvaluationCheck(): allows for a very fast capacity-check
// based on given working-set sizes that can be trivially derived
// by the caller. The more powerful Evaluate() function also
// performs these checks, but computes both tile sizes and access counts
// and requires full tiling data that is generated by a very slow
// Nest::ComputeWorkingSets() algorithm. The PreEvaluationCheck()
// function is an optional call that extensive design-space searches
// can use to fail early.
// FIXME: integrate with Evaluate() and re-factor.
// FIXME: what about instances and fanout checks?
EvalStatus BufferLevel::PreEvaluationCheck(
  const problem::PerDataSpace<std::size_t> working_set_sizes,
  const tiling::CompoundMask mask,
  const problem::Workload* workload,
  const bool break_on_failure)
{
  (void) break_on_failure;

  bool success = true;
  std::ostringstream fail_reason;
  
  if (specs_.size.IsSpecified())
  {
    // Ugh. If we can do a distributed multicast from this level,
    // then the required size may be smaller. However, that depends
    // on the multicast factor etc. that we don't know at this point.
    // Use a very loose filter and fail this check only if there's
    // no chance that this mapping can fit.
    auto available_capacity = specs_.effective_size.Get();
    if (network_read_->DistributedMulticastSupported())
    {
      available_capacity *= specs_.instances.Get();
    }

    // Find the total capacity required by all un-masked data types.
    std::size_t required_capacity = 0;
    for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
    {
      if (mask[pvi])
      {
        auto dense_working_set_size = working_set_sizes.at(problem::Shape::DataSpaceID(pvi));
        auto sparse_working_set_size = ceil(dense_working_set_size * workload->GetDensity(pvi).GetTileExpectedDensity(dense_working_set_size));
        // required_capacity += working_set_sizes.at(problem::Shape::DataSpaceID(pvi));
        required_capacity += sparse_working_set_size;
      }
    }

    if (required_capacity > available_capacity)
    {
      success = false;
      fail_reason << "mapped tile size " << required_capacity << " exceeds buffer capacity "
                  << available_capacity;
    }
    else if (required_capacity < specs_.effective_size.Get()
             * specs_.min_utilization.Get())
    {
      success = false;
      fail_reason << "mapped tile size " << required_capacity << " is less than constrained "
                  << "minimum utilization " << specs_.effective_size.Get() * specs_.min_utilization.Get();
    }
  }

  EvalStatus eval_status;
  eval_status.success = success;
  eval_status.fail_reason = fail_reason.str();

  return eval_status;  
}

//
// Heavyweight Evaluate() function.
// FIXME: Derive FanoutX, FanoutY, MeshX, MeshY from mapping if unspecified.
//
EvalStatus BufferLevel::Evaluate(const tiling::CompoundTile& tile, const tiling::CompoundMask& mask,
                                 const std::uint64_t compute_cycles,
                                 const bool break_on_failure)
{
  auto eval_status = ComputeAccesses(tile.data_movement_info, mask, break_on_failure);
  if (!break_on_failure || eval_status.success)
  {
    ComputeBufferEnergy(tile.data_movement_info);
    ComputeReductionEnergy();
    ComputeAddrGenEnergy();
    ComputePerformance(compute_cycles);
  }
  return eval_status;
}

bool BufferLevel::HardwareReductionSupported()
{
  // FIXME: take this information from an explicit arch spec.
  return !(specs_.technology.IsSpecified() &&
           specs_.technology.Get() == Technology::DRAM);
}

void BufferLevel::ConnectRead(std::shared_ptr<Network> network)
{
  network_read_ = network;
}

void BufferLevel::ConnectFill(std::shared_ptr<Network> network)
{
  network_fill_ = network;
}

void BufferLevel::ConnectUpdate(std::shared_ptr<Network> network)
{
  network_update_ = network;
}

void BufferLevel::ConnectDrain(std::shared_ptr<Network> network)
{
  network_drain_ = network;
}


uint64_t GetMetaDataTileSize(tiling::DataMovementInfo per_datatype_tile_info, double tile_density){

    uint64_t metadata_tile_size;

    // compute the corresponding metadata tile size
    if (per_datatype_tile_info.metadata_format == "bitmask"){
       metadata_tile_size = per_datatype_tile_info.size;

    } else if (per_datatype_tile_info.metadata_format == "RLE"){
       // metadata_tile_size = compressed_tile_size;
       metadata_tile_size = ceil(per_datatype_tile_info.size * tile_density);

    } else if (per_datatype_tile_info.metadata_format == "CSR"){
       metadata_tile_size = per_datatype_tile_info.dense_rank1_fills
                            + per_datatype_tile_info.dense_rank0_fills * tile_density;

    } else {
       metadata_tile_size = 0;
    }

    return metadata_tile_size;
}


EvalStatus BufferLevel::ComputeAccesses(const tiling::CompoundDataMovementInfo& tile,
                                        const tiling::CompoundMask& mask,
                                        const bool break_on_failure)
{
  (void) break_on_failure;

  bool success = true;
  std::ostringstream fail_reason;
  
  // Subnest FSM should be same for each problem::Shape::DataSpaceID in the list,
  // so just copy it from datatype #0.
  subnest_ = tile[0].subnest;

  //
  // 1. Collect stats (stats are always collected per-DataSpaceID).
  //

  uint64_t total_tile_size = 0;
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++){
      total_tile_size += tile[pvi].size;
      total_tile_size += ceil(GetMetaDataTileSize(tile[pvi], tile[pvi].tile_density.GetTileExpectedDensity(tile[pvi].size))
                              * 1.0 * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());
  }

  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);

    stats_.keep[pv] = mask[pv];
    
    stats_.partition_size[pv] = tile[pvi].partition_size;
    // stats_.utilized_capacity[pv] = tile[pvi].size;
    stats_.tile_size[pv] = tile[pvi].size;
    // assume metadata is stored in the same storage as the actual data
    // stats_.metadata_tile_size[pv] = tile[pvi].metadata_tile_size;

    double tile_confidence = 1.0;
    uint64_t compressed_tile_size = 0;
    uint64_t metadata_tile_size = 0;
    double stored_data_density = 1.0;

    // compute compressed tile size
    if (tile[pvi].compressed){

      if (tile[pvi].tile_density.user_defined_knob){

         tile_confidence = tile[pvi].tile_density.GetUserDefinedConfidence();
         stored_data_density = tile[pvi].tile_density.GetTileDensityByConfidence(tile[pvi].size, tile_confidence);
         compressed_tile_size = ceil((double)tile[pvi].size * stored_data_density);
         metadata_tile_size = GetMetaDataTileSize(tile[pvi], stored_data_density);

      } else {

        if (specs_.effective_size.IsSpecified()){

          uint64_t allocated_effective_buffer_size;

          uint64_t equivalent_metadata_tile_size;
          uint64_t updated_equivalent_metadata_tile_size;
          metadata_tile_size = GetMetaDataTileSize(tile[pvi], tile[pvi].tile_density.GetTileExpectedDensity(tile[pvi].size));
          equivalent_metadata_tile_size = ceil((double)metadata_tile_size * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());

          if (total_tile_size != 0){
            allocated_effective_buffer_size = ceil(specs_.effective_size.Get() *
                                                   (tile[pvi].size + equivalent_metadata_tile_size)/ total_tile_size);
          } else {
            allocated_effective_buffer_size = specs_.effective_size.Get() ;
          }


//          std::cout << "======> level name: " << specs_.name.Get() << std::endl;
//          std::cout << "======> total buffer size: " << allocated_effective_buffer_size << std::endl;
//          std::cout << "======> tile shape: " << tile[pvi].size << std::endl;
//          std::cout << "expected metadata equival: " << equivalent_metadata_tile_size << std::endl;
//          std::cout << "buffer size 1: " << allocated_effective_buffer_size - equivalent_metadata_tile_size << std::endl;
          tile_confidence = tile[pvi].tile_density.GetTileConfidence(tile[pvi].size, allocated_effective_buffer_size - equivalent_metadata_tile_size);
//          std::cout << "tile_confidence: " << tile_confidence  << std::endl;
          stored_data_density = tile[pvi].tile_density.GetTileDensityByConfidence(tile[pvi].size,
                                                                                  tile_confidence,
                                                                                  allocated_effective_buffer_size - equivalent_metadata_tile_size);
          compressed_tile_size = ceil(tile[pvi].size * stored_data_density);
//          std::cout << " stored_data_density: "<<  stored_data_density << " compressed size 1: " << compressed_tile_size << std::endl;
          metadata_tile_size = GetMetaDataTileSize(tile[pvi], stored_data_density);
          equivalent_metadata_tile_size = ceil((double)metadata_tile_size * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());
//          std::cout  << " --> name: " << specs_.name.Get() << "  metadata format: " << tile[pvi].metadata_format << "  metadata tile size 1: " << metadata_tile_size
//                     << "  equivalent_metadata_tile_size: " << equivalent_metadata_tile_size
//                     << "  stored_data_density: "<<  stored_data_density << "  tile_size: " << tile[pvi].size << " compressed_tile_size: " << compressed_tile_size
//                     << " total buffer size: " << allocated_effective_buffer_size <<std::endl;

          // if the data tile takes to much space, regenerate a conservative estimation
          if (equivalent_metadata_tile_size + compressed_tile_size > allocated_effective_buffer_size && tile_confidence != 0){

             // use -1 here for buffer size to prevent failure for cases when the percentile number is rounded up by one in the GetTileConfidence's
             // quantile function
             tile_confidence = tile[pvi].tile_density.GetTileConfidence(tile[pvi].size, allocated_effective_buffer_size - equivalent_metadata_tile_size-1);
             stored_data_density = tile[pvi].tile_density.GetTileDensityByConfidence(tile[pvi].size,
                                                                                     tile_confidence,
                                                                                     allocated_effective_buffer_size - equivalent_metadata_tile_size-1);
//             std::cout << "buffer size 2: " << allocated_effective_buffer_size - equivalent_metadata_tile_size
//                       << "  tile_confidence 2: " << tile_confidence << " stored_data_density 2: " << stored_data_density << std::endl;
             compressed_tile_size = ceil(tile[pvi].size * stored_data_density);
//             std::cout << "  compressed size 2: " << compressed_tile_size << std::endl;

             metadata_tile_size = GetMetaDataTileSize(tile[pvi], stored_data_density);
             updated_equivalent_metadata_tile_size = ceil((double)metadata_tile_size * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());
//             std::cout << "metadata_tile_size 2: " << metadata_tile_size << std::endl;

             assert(updated_equivalent_metadata_tile_size + compressed_tile_size <= allocated_effective_buffer_size);

             uint64_t tmp_metadata_tile_size;
             double tmp_tile_confidence;
             double tmp_stored_data_density;
             uint64_t tmp_compressed_tile_size;

             while(updated_equivalent_metadata_tile_size + compressed_tile_size <= 0.99 * allocated_effective_buffer_size &&
                   updated_equivalent_metadata_tile_size != equivalent_metadata_tile_size){

                equivalent_metadata_tile_size = updated_equivalent_metadata_tile_size;
                tmp_tile_confidence = tile[pvi].tile_density.GetTileConfidence(tile[pvi].size, allocated_effective_buffer_size - equivalent_metadata_tile_size);
                tmp_stored_data_density = tile[pvi].tile_density.GetTileDensityByConfidence(tile[pvi].size,
                                                                                            tmp_tile_confidence,
                                                                                            allocated_effective_buffer_size - equivalent_metadata_tile_size);
                tmp_compressed_tile_size = ceil(tile[pvi].size * tmp_stored_data_density);
                tmp_metadata_tile_size = GetMetaDataTileSize(tile[pvi], tmp_stored_data_density);

                updated_equivalent_metadata_tile_size = ceil(tmp_metadata_tile_size * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());
//                std::cout << "compressed tile size: " << tmp_compressed_tile_size << " metadata_equival_size: " << updated_equivalent_metadata_tile_size << std::endl;

                if (updated_equivalent_metadata_tile_size + tmp_compressed_tile_size > allocated_effective_buffer_size){
                    updated_equivalent_metadata_tile_size = equivalent_metadata_tile_size;
                }

                if (updated_equivalent_metadata_tile_size != equivalent_metadata_tile_size){
                    metadata_tile_size = tmp_metadata_tile_size;
                    compressed_tile_size = tmp_compressed_tile_size;
                    stored_data_density = tmp_stored_data_density;
                    tile_confidence = tmp_tile_confidence;
                }
             }
//
             assert(updated_equivalent_metadata_tile_size + compressed_tile_size <= allocated_effective_buffer_size);
          }
//          std::cout <<"   compresssed: " << tile[pvi].compressed
//                    << "  metadata tile size: " << metadata_tile_size << std::endl;
        } else {
          // infinite memory size, e.g., DRAM, can fit for sure
          tile_confidence = 1.0;
          stored_data_density = tile[pvi].tile_density.GetTileExpectedDensity(tile[pvi].size);
          compressed_tile_size = ceil(tile[pvi].size * stored_data_density);
          metadata_tile_size = GetMetaDataTileSize(tile[pvi], stored_data_density);
        }
      }

//       std::cout << "tile_confidence: " << tile_confidence << "  stored_data_density: " << stored_data_density << std::endl;

    } else { // no compression

     if (tile[pvi].metadata_format == "bitmask"){
       metadata_tile_size = tile[pvi].size;
     }
     compressed_tile_size = tile[pvi].size;
    }

    stats_.tile_confidence[pv] = tile_confidence;
    stats_.compressed_tile_size[pv] = compressed_tile_size;
    stats_.metadata_tile_size[pv] = metadata_tile_size;
    stats_.tile_max_density[pv] = stored_data_density;

//    std::cout << stats_.tile_confidence[pv] << "   uncompressed tile size: " << stats_.tile_size[pv]
//                                            << "   tile size: " << stats_.compressed_tile_size[pv]
//                                            << "   metadata tile size: " << stats_.metadata_tile_size[pv] << std::endl;


//    stats_.utilized_capacity[pv] = tile[pvi].compressed_size
//                                   + ceil(tile[pvi].metadata_tile_size
//                                          * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());

    stats_.utilized_capacity[pv] = compressed_tile_size
                                   + ceil((double)metadata_tile_size
                                          * specs_.metadata_word_bits.Get() / specs_.word_bits.Get());
    stats_.utilized_instances[pv] = tile[pvi].replication_factor;

    assert((tile[pvi].size == 0) == (tile[pvi].content_accesses == 0));

    //
    // the commented calculations below is now moved to tiling.cpp
    //
   
    // if (problem::GetShape()->IsReadWriteDataSpace.at(pv))
    // {
    //   // First epoch is an Update, all subsequent epochs are Read-Modify-Update.

    //   // The following assertion is *incorrect* for coefficients (e.g. stride, pad) > 1.
    //   // FIXME: find a safety check that works with coefficients > 1.
    //   // assert(tile[pvi].size == 0 || tile[pvi].content_accesses % tile[pvi].size == 0);

    //   stats_.reads[pv] = tile[pvi].content_accesses - tile[pvi].partition_size + tile[pvi].peer_accesses;
    //   stats_.updates[pv] = tile[pvi].content_accesses;
    //   stats_.fills[pv] = tile[pvi].fills + tile[pvi].peer_fills;
    //   stats_.address_generations[pv] = stats_.updates[pv] + stats_.fills[pv]; // scalar

    //   // FIXME: temporal reduction and network costs if hardware reduction isn't
    //   // supported appears to be wonky - network costs may need to trickle down
    //   // all the way to the level that has the reduction hardware.
    //   stats_.temporal_reductions[pv] = tile[pvi].content_accesses - tile[pvi].partition_size;
    //   std::cout << "stats: reads, updates, fills, address_generations " 
    //   << stats_.reads[pv] << " " << stats_.updates[pv]<< " " << stats_.fills[pv] << " " << stats_.address_generations[pv] <<std::endl;
    // }
    // else // Read-only data type.
    // {
    //   stats_.reads[pv] = tile[pvi].content_accesses + tile[pvi].peer_accesses;
    //   stats_.updates[pv] = 0;
    //   stats_.fills[pv] = tile[pvi].fills + tile[pvi].peer_fills;
    //   stats_.address_generations[pv] = stats_.reads[pv] + stats_.fills[pv]; // scalar
    //   stats_.temporal_reductions[pv] = 0;
    // }
    // original high-level actions
    stats_.reads[pv] = tile[pvi].reads;
    stats_.updates[pv] = tile[pvi].updates;
    stats_.fills[pv] = tile[pvi].fills;
    stats_.temporal_reductions[pv] = tile[pvi].temporal_reductions;
    if (problem::GetShape()->IsReadWriteDataSpace.at(pv)) 
      stats_.address_generations[pv] = stats_.updates[pv] + stats_.fills[pv]; // FIXME? we want address generation be accounted for in energy/compound action?
    else
      stats_.address_generations[pv] = stats_.reads[pv] + stats_.fills[pv]; // FIXME? we want address generation be accounted for in energy/compound action?

    stats_.metadata_reads[pv] = tile[pvi].metadata_reads;
    stats_.metadata_fills[pv] = tile[pvi].metadata_fills;
    stats_.metadata_updates[pv] = tile[pvi].metadata_updates;

    // record the access counts for fine-grained actions
    stats_.gated_reads[pv] = tile[pvi].fine_grained_accesses.at("gated_read");
    stats_.skipped_reads[pv] = tile[pvi].fine_grained_accesses.at("skipped_read");
    stats_.random_reads[pv] = tile[pvi].fine_grained_accesses.at("random_read");

    stats_.gated_fills[pv] = tile[pvi].fine_grained_accesses.at("gated_fill");
    stats_.skipped_fills[pv] = tile[pvi].fine_grained_accesses.at("skipped_fill");
    stats_.random_fills[pv] = tile[pvi].fine_grained_accesses.at("random_fill");

    stats_.random_updates[pv] = tile[pvi].fine_grained_accesses.at("random_update");
    stats_.gated_updates[pv] = tile[pvi].fine_grained_accesses.at("gated_update");
    stats_.skipped_updates[pv] = tile[pvi].fine_grained_accesses.at("skipped_update");

    stats_.random_metadata_reads[pv] =  tile[pvi].fine_grained_accesses.at("metadata_read");
    stats_.gated_metadata_reads[pv] =  tile[pvi].fine_grained_accesses.at("gated_metadata_read");

    stats_.random_metadata_fills[pv] =  tile[pvi].fine_grained_accesses.at("metadata_fill");
    stats_.gated_metadata_fills[pv] =  tile[pvi].fine_grained_accesses.at("gated_metadata_fill");

    stats_.random_metadata_updates[pv] =  tile[pvi].fine_grained_accesses.at("metadata_update");
    stats_.gated_metadata_updates[pv] =  tile[pvi].fine_grained_accesses.at("gated_metadata_update");

    stats_.decompression_counts[pv] =  tile[pvi].fine_grained_accesses.at("decompression_count");
    stats_.compression_counts[pv] =  tile[pvi].fine_grained_accesses.at("compression_count");
  }

  //
  // 2. Derive/validate architecture specs based on stats.
  //      
  auto total_utilized_capacity = std::accumulate(stats_.utilized_capacity.begin(),
                                                 stats_.utilized_capacity.end(),
                                                 0ULL);
  if (!specs_.size.IsSpecified())
  {
#ifdef UPDATE_UNSPECIFIED_SPECS
    specs_.size = std::ceil(total_utilized_capacity * specs_.multiple_buffering.Get());
#endif
  }
  else if (total_utilized_capacity > specs_.effective_size.Get())
  {
    success = false;
    fail_reason << "mapped tile size " << total_utilized_capacity << " exceeds buffer capacity "
                << specs_.effective_size.Get();
  }
  else if (total_utilized_capacity < specs_.effective_size.Get()
           * specs_.min_utilization.Get())
  {
    success = false;
    fail_reason << "mapped tile size " << total_utilized_capacity << " is less than constrained "
                << "minimum utilization " << specs_.effective_size.Get() * specs_.min_utilization.Get();
  }

  assert (specs_.block_size.IsSpecified());
    
  assert (specs_.cluster_size.IsSpecified());
   
  // Compute address-generation bits.
  if (specs_.size.IsSpecified())
  {
    double address_range = std::ceil(static_cast<double>(specs_.size.Get() / specs_.block_size.Get()));
    specs_.addr_gen_bits = static_cast<unsigned long>(std::ceil(std::log2(address_range)));
  }
  else if (specs_.technology.Get() == Technology::SRAM)
  {
    // Use utilized capacity as proxy for size.
    double address_range = std::ceil(static_cast<double>(total_utilized_capacity / specs_.block_size.Get()));
    specs_.addr_gen_bits = static_cast<unsigned long>(std::ceil(std::log2(address_range)));
  }
  else // DRAM.
  {
#ifdef FIXED_DRAM_SIZE_IF_UNSPECIFIED
    // DRAM of un-specified size, use 48-bit physical address.
    specs_.addr_gen_bits = 48;
#else
    // Use utilized capacity as proxy for size.
    double address_range = std::ceil(static_cast<double>(total_utilized_capacity / specs_.block_size.Get()));
    specs_.addr_gen_bits = static_cast<unsigned long>(std::ceil(std::log2(address_range)));
#endif
  }
  if (!specs_.instances.IsSpecified())
  {
#ifdef UPDATE_UNSPECIFIED_SPECS
    specs_.instances = stats_.utilized_instances.Max();
#endif
  }
  else if (stats_.utilized_instances.Max() > specs_.instances.Get())
  {
    success = false;
    fail_reason << "mapped instances " << stats_.utilized_instances.Max() << " exceeds available hardware instances "
                << specs_.instances.Get();
  }

  // Bandwidth constraints cannot be checked/inherited at this point
  // because the calculation is a little more involved. We will do
  // this later in the ComputePerformance() function.      

  // Compute utilized clusters.
  // FIXME: should derive this from precise spatial mapping.
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    // The following equation assumes fully condensed mapping. Do a ceil-div.
    // stats_.utilized_clusters[pv] = 1 + (stats_.utilized_instances[pv] - 1) /
    //    specs_.cluster_size.Get();
    // Assume utilized instances are sprinkled uniformly across all clusters.
    auto num_clusters = specs_.instances.Get() / specs_.cluster_size.Get();
    stats_.utilized_clusters[pv] = std::min(stats_.utilized_instances[pv],
                                            num_clusters);
  }

  is_evaluated_ = success;

  EvalStatus eval_status;
  eval_status.success = success;
  eval_status.fail_reason = fail_reason.str();
    
  return eval_status;
}

// Compute buffer energy.
void BufferLevel::ComputeBufferEnergy(const tiling::CompoundDataMovementInfo& data_movement_info)
{
  // NOTE! Stats are always maintained per-DataSpaceID
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    auto instance_accesses = stats_.reads.at(pv) + stats_.updates.at(pv) + stats_.fills.at(pv);

    auto block_size = specs_.block_size.Get();
    double vector_accesses =
      (instance_accesses % block_size == 0) ?
      (instance_accesses / block_size)      :
      (instance_accesses / block_size) + 1;

    // compute for meta data accesses
    auto instance_metadata_accesses = stats_.metadata_reads[pv] + stats_.metadata_fills[pv] + stats_.metadata_updates[pv];
    auto metadata_block_size = specs_.metadata_block_size.Get();
    double metadata_vector_accesses =
      (instance_metadata_accesses % metadata_block_size == 0) ?
      (instance_metadata_accesses / metadata_block_size)      :
      (instance_metadata_accesses / metadata_block_size) + 1;


    // double cluster_access_energy = vector_accesses *
    //   specs_.vector_access_energy.Get();

    // compute in terms of fine-grained action types
    std::string op_name;
    double cluster_access_energy = 0;
    for (unsigned op_id = 0; op_id < unsigned( tiling::GetNumOpTypes("storage")); op_id++){
        op_name = tiling::storageOperationTypes[op_id];
        
        if (op_name.find("metadata") == std::string::npos && op_name.find("count") == std::string::npos) { // data storage related computations

          // get the number of each fine-grained vector accesses according to original access ratio
          if (instance_accesses != 0){
            cluster_access_energy += vector_accesses*data_movement_info[pv].fine_grained_accesses.at(op_name)/instance_accesses * specs_.op_energy_map.at(op_name);
          } else {
            cluster_access_energy += 0;
          }
        
        } else if (op_name.find("count") == std::string::npos){ // metadata storage related computations

          if (instance_metadata_accesses != 0){
            cluster_access_energy += metadata_vector_accesses * data_movement_info[pv].fine_grained_accesses.at(op_name)
                                     /instance_metadata_accesses
                                     * specs_.op_energy_map.at(op_name);
//            std::cout << "level name: " << specs_.name.Get() << std::endl;
//            std::cout << "op name: " << op_name << std::endl;
//            std::cout << "op energy: " << specs_.op_energy_map.at(op_name) << std::endl;
//            std::cout << "total energy:  " << cluster_access_energy << std::endl;

          } else {
            cluster_access_energy += 0;
          }

        } else { // decompression/compression energy
          cluster_access_energy += data_movement_info[pv].fine_grained_accesses.at(op_name) * specs_.op_energy_map.at(op_name);
        }
    }

    uint64_t cluster_speculation_energy_cost;
    stats_.parent_level_name[pvi] = "";

    if (data_movement_info[pvi].parent_level != std::numeric_limits<unsigned>::max()){
       stats_.parent_level_name[pvi] = data_movement_info[pvi].parent_level_name;
    }

    if (stats_.tile_confidence[pvi] != 1.0 && stats_.parent_level_name[pvi] != ""){
          stats_.parent_level_name[pvi] = data_movement_info[pvi].parent_level_name;
          double parent_scalar_read_energy = data_movement_info[pvi].parent_level_op_energy.at("random_read")/data_movement_info[pvi].parent_level_simple_specs.at("block_size");
          double child_scalar_read_energy = specs_.op_energy_map.at("random_read")/specs_.block_size.Get();
//          std::cout <<"parent child ratio: " << parent_scalar_read_energy/child_scalar_read_energy << std::endl;
//          std::cout << "confidence: " << stats_.tile_confidence[pvi] << std::endl;
           cluster_speculation_energy_cost = ceil(cluster_access_energy * (1-stats_.tile_confidence[pvi]) * (parent_scalar_read_energy/child_scalar_read_energy));
//           std::cout << "cluster_speculation_energy_cost: " << cluster_speculation_energy_cost << std::endl;
           cluster_access_energy = cluster_access_energy * (stats_.tile_confidence[pvi]);
//           std::cout << "weighted cluster_access_energy: " << cluster_access_energy << std::endl;
    } else {

       cluster_speculation_energy_cost = 0;
    }

//    std::cout << "name: " << specs_.name.Get() << " internal energy: " << cluster_access_energy
//              << " tile confidence: " << stats_.tile_confidence[pvi] << std::endl;
    // Spread out the cost between the utilized instances in each cluster.
    // This is because all the later stat-processing is per-instance.
    if (stats_.utilized_instances.at(pv) > 0)
    {
      double cluster_utilization = double(stats_.utilized_instances.at(pv)) /
      double(stats_.utilized_clusters.at(pv));
      stats_.speculation_energy_cost[pv]  = cluster_speculation_energy_cost / cluster_utilization;
      stats_.energy[pv] = (cluster_access_energy + cluster_speculation_energy_cost) / cluster_utilization;
      stats_.energy_per_access[pv] = stats_.energy.at(pv) / instance_accesses;
    }
    else
    {
      stats_.energy[pv] = 0;
      stats_.energy_per_access[pv] = 0;
    }
  }
}

//
// Compute reduction energy.
//
void BufferLevel::ComputeReductionEnergy()
{
  // Temporal reduction: add a value coming in on the network to a value stored locally.
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    if (problem::GetShape()->IsReadWriteDataSpace.at(pv))
    {
      stats_.temporal_reduction_energy[pv] = stats_.temporal_reductions[pv] * 
        pat::AdderEnergy(specs_.word_bits.Get(), network_update_->WordBits());
    }
    else
    {
      stats_.temporal_reduction_energy[pv] = 0;
    }
  }
}

//
// Compute address generation energy.
//
void BufferLevel::ComputeAddrGenEnergy()
{
  // Note! Address-generation is amortized across the cluster width.
  // We compute the per-cluster energy here. When we sum across instances,
  // we need to be careful to only count each cluster once.
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    // We'll use an addr-gen-bits + addr-gen-bits adder, though
    // it's probably cheaper than that. However, we can't assume
    // a 1-bit increment.
    auto pv = problem::Shape::DataSpaceID(pvi);
    if (specs_.addr_gen_energy.Get() < 0.0) { 
      stats_.addr_gen_energy[pv] = stats_.address_generations[pv] *
        pat::AdderEnergy(specs_.addr_gen_bits.Get(), specs_.addr_gen_bits.Get());
    }
    else
    {
       stats_.addr_gen_energy[pv] = stats_.address_generations[pv] * specs_.addr_gen_energy.Get();
    }
  }
}

//
// Compute performance.
//
void BufferLevel::ComputePerformance(const std::uint64_t compute_cycles)
{
  //
  // Step 1: Compute unconstrained bandwidth demand.
  //
  problem::PerDataSpace<double> unconstrained_read_bandwidth;
  problem::PerDataSpace<double> unconstrained_write_bandwidth;
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    auto total_read_accesses    =   stats_.reads.at(pv);
    auto total_write_accesses   =   stats_.updates.at(pv) + stats_.fills.at(pv);
    unconstrained_read_bandwidth[pv]  = (double(total_read_accesses)  / compute_cycles);
    unconstrained_write_bandwidth[pv] = (double(total_write_accesses) / compute_cycles);
  }

  //
  // Step 2: Compare vs. specified bandwidth and calculate slowdown.
  //
  stats_.slowdown = 1.0;

  // Find slowdown.
  auto total_unconstrained_read_bandwidth  = std::accumulate(unconstrained_read_bandwidth.begin(),  unconstrained_read_bandwidth.end(),  0.0);
  auto total_unconstrained_write_bandwidth = std::accumulate(unconstrained_write_bandwidth.begin(), unconstrained_write_bandwidth.end(), 0.0);

  if (specs_.read_bandwidth.IsSpecified() &&
      specs_.read_bandwidth.Get() < total_unconstrained_read_bandwidth)
  {
    stats_.slowdown =
      std::min(stats_.slowdown,
               specs_.read_bandwidth.Get() / total_unconstrained_read_bandwidth);
  }
  if (specs_.write_bandwidth.IsSpecified() &&
      specs_.write_bandwidth.Get() < total_unconstrained_write_bandwidth)
  {
    stats_.slowdown =
      std::min(stats_.slowdown,
               specs_.write_bandwidth.Get() / total_unconstrained_write_bandwidth);
  }

  //
  // Step 3:
  // Calculate real bandwidths based on worst slowdown. For shared buffers this
  // ends up effectively slowing down each datatype's bandwidth by the slowdown
  // amount, which is slightly weird but appears to be harmless.
  //
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    stats_.read_bandwidth[pv]  = stats_.slowdown * unconstrained_read_bandwidth.at(pv);
    stats_.write_bandwidth[pv] = stats_.slowdown * unconstrained_write_bandwidth.at(pv);
  }

  //
  // Step 4: Calculate execution cycles.
  //
  stats_.cycles = std::uint64_t(ceil(compute_cycles / stats_.slowdown));

  //
  // Step 5: Update arch specs.
  //
#ifdef UPDATE_UNSPECIFIED_SPECS
  if (!specs_.read_bandwidth.IsSpecified())
    specs_.read_bandwidth = std::accumulate(stats_.read_bandwidth.begin(), stats_.read_bandwidth.end(), 0.0);
  if (!specs_.write_bandwidth.IsSpecified())
    specs_.write_bandwidth = std::accumulate(stats_.write_bandwidth.begin(), stats_.write_bandwidth.end(), 0.0);
#endif
}

//
// Accessors.
//

STAT_ACCESSOR(double, BufferLevel, StorageEnergy, stats_.energy.at(pv) * stats_.utilized_instances.at(pv))
STAT_ACCESSOR(double, BufferLevel, TemporalReductionEnergy, stats_.temporal_reduction_energy.at(pv) * stats_.utilized_instances.at(pv))
STAT_ACCESSOR(double, BufferLevel, AddrGenEnergy, stats_.addr_gen_energy.at(pv) * stats_.utilized_clusters.at(pv)) // Note!!! clusters, not instances.
STAT_ACCESSOR(double, BufferLevel, Energy,
              StorageEnergy(pv) +
              TemporalReductionEnergy(pv) +
              AddrGenEnergy(pv))

STAT_ACCESSOR(std::uint64_t, BufferLevel, Accesses, stats_.utilized_instances.at(pv) * (stats_.reads.at(pv) + stats_.updates.at(pv) + stats_.fills.at(pv)))
STAT_ACCESSOR(std::uint64_t, BufferLevel, UtilizedCapacity, stats_.utilized_capacity.at(pv))
STAT_ACCESSOR(std::uint64_t, BufferLevel, TileSize, stats_.tile_size.at(pv))
STAT_ACCESSOR(std::uint64_t, BufferLevel, UtilizedInstances, stats_.utilized_instances.at(pv))

std::string BufferLevel::Name() const
{
  return specs_.name.Get();
}

double BufferLevel::Area() const
{
  double area = 0;
  area += specs_.storage_area.Get() * specs_.instances.Get();
  return area;
}

double BufferLevel::AreaPerInstance() const
{
  double area = 0;
  area += specs_.storage_area.Get();
  return area;
}

double BufferLevel::Size() const
{
  // FIXME: this is per-instance. This is inconsistent with the naming
  // convention of some of the other methods, which are summed across instances.
  double size = 0;
  size += specs_.size.Get();
  return size;
}

double BufferLevel::CapacityUtilization() const
{
  double utilized_capacity = 0;
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);
    utilized_capacity += stats_.utilized_capacity.at(pv) *
      stats_.utilized_instances.at(pv);
  }

  double total_capacity = Size() * specs_.instances.Get();

  return utilized_capacity / total_capacity;
}

std::uint64_t BufferLevel::Cycles() const
{
  return stats_.cycles;
}

// ---------------
//    Printers
// ---------------

std::ostream& operator<<(std::ostream& out, const BufferLevel::Technology& tech)
{
  switch (tech)
  {
    case BufferLevel::Technology::SRAM: out << "SRAM"; break;
    case BufferLevel::Technology::DRAM: out << "DRAM"; break;
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const BufferLevel& buffer_level)
{
  buffer_level.Print(out);
  return out;
}

void BufferLevel::Print(std::ostream& out) const
{
  std::string indent = "    ";

  auto& specs = specs_;
  auto& stats = stats_;

  // Print level name.
  out << "=== " << specs.level_name << " ===" << std::endl;  
  out << std::endl;

  // Print specs.
  out << indent << "SPECS" << std::endl;
  out << indent << "-----" << std::endl;

// flag to print verbose sparse stats or dense stats
// #define PRINT_SPARSE_STATS
#ifdef PRINT_SPARSE_STATS
  out << indent << indent << "Technology                   : " << specs.technology << std::endl;
  out << indent << indent << "Size                         : " << specs.size << std::endl;
  out << indent << indent << "Word bits                    : " << specs.word_bits << std::endl;
  out << indent << indent << "Block size                   : " << specs.block_size << std::endl;
  out << indent << indent << "Metadata word bits           : " << specs.metadata_word_bits << std::endl;
  out << indent << indent << "Metadata block size          : " << specs.metadata_block_size << std::endl;
  out << indent << indent << "Cluster size                 : " << specs.cluster_size << std::endl;
  out << indent << indent << "Instances                    : " << specs.instances << " ("
      << specs.meshX << "*" << specs.meshY << ")" << std::endl;
  out << indent << indent << "Read bandwidth               : " << specs.read_bandwidth << std::endl;    
  out << indent << indent << "Write bandwidth              : " << specs.write_bandwidth << std::endl;    
  out << indent << indent << "Multiple buffering           : " << specs.multiple_buffering << std::endl;
  out << indent << indent << "Effective size               : " << specs.effective_size << std::endl;
  out << indent << indent << "Min utilization              : " << specs.min_utilization << std::endl;
  out << indent << indent << "Vector access energy(max)    : " << specs.vector_access_energy << " pJ" << std::endl;
  out << indent << indent << "Vector gated read energy     : " << specs.op_energy_map.at("gated_read") << " pJ" << std::endl;
  out << indent << indent << "Vector skipped read energy   : " << specs.op_energy_map.at("skipped_read") << " pJ" << std::endl;
  out << indent << indent << "Vector random read energy    : " << specs.op_energy_map.at("random_read") << " pJ" << std::endl;
  out << indent << indent << "Vector gated write energy    : " << specs.op_energy_map.at("gated_fill") << " pJ" << std::endl;
  out << indent << indent << "Vector skipped write energy  : " << specs.op_energy_map.at("skipped_fill") << " pJ" << std::endl;
  out << indent << indent << "Vector random write energy   : " << specs.op_energy_map.at("random_fill") << " pJ" << std::endl;
//  out << indent << indent << "Vector gated update energy   : " << specs.op_energy_map.at("gated_update") << " pJ" << std::endl;
//  out << indent << indent << "Vector skipped update energy : " << specs.op_energy_map.at("skipped_update") << " pJ" << std::endl;
//  out << indent << indent << "Vector random update energy  : " << specs.op_energy_map.at("random_update") << " pJ" << std::endl;
  out << indent << indent << "Vector metadata read energy  : " << specs.op_energy_map.at("metadata_read") << " pJ" << std::endl;
  out << indent << indent << "Vector metadata write energy : " << specs.op_energy_map.at("metadata_fill") << " pJ" << std::endl;
  out << indent << indent << "(De)compression energy       : " << specs.op_energy_map.at("decompression_count") << " pJ" << std::endl;
  out << indent << indent << "Area                         : " << specs.storage_area << " um^2" << std::endl;

  out << std::endl;

#else
  out << indent << indent << "Technology           : " << specs.technology << std::endl;
  out << indent << indent << "Size                 : " << specs.size << std::endl;
  out << indent << indent << "Word bits            : " << specs.word_bits << std::endl;
  out << indent << indent << "Block size           : " << specs.block_size << std::endl;
  out << indent << indent << "Cluster size         : " << specs.cluster_size << std::endl;
  out << indent << indent << "Instances            : " << specs.instances << " ("
      << specs.meshX << "*" << specs.meshY << ")" << std::endl;
  out << indent << indent << "Read bandwidth       : " << specs.read_bandwidth << std::endl;
  out << indent << indent << "Write bandwidth      : " << specs.write_bandwidth << std::endl;
  out << indent << indent << "Multiple buffering   : " << specs.multiple_buffering << std::endl;
  out << indent << indent << "Effective size       : " << specs.effective_size << std::endl;
  out << indent << indent << "Min utilization      : " << specs.min_utilization << std::endl;
  out << indent << indent << "Vector access energy : " << specs.vector_access_energy << " pJ" << std::endl;
  out << indent << indent << "Area                 : " << specs.storage_area << " um^2" << std::endl;

  out << std::endl;
#endif

  // If the buffer hasn't been evaluated on a specific mapping yet, return.
  if (!IsEvaluated())
  {
    return;
  }

  // Print mapping.
  out << indent << "MAPPING" << std::endl;
  out << indent << "-------" << std::endl;
  out << indent << "Loop nest:" << std::endl;
  std::string loopindent = "  ";
  for (auto loop = subnest_.rbegin(); loop != subnest_.rend(); loop++)
  {
    // Do not print loop if it's a trivial factor.
    if ((loop->start + loop->stride) < loop->end)
    {
      out << indent << loopindent << *loop << std::endl;
      loopindent += "  ";
    }
  }
  out << std::endl;

  // Print stats.
  out << indent << "STATS" << std::endl;
  out << indent << "-----" << std::endl;

  out << indent << "Cycles               : " << stats.cycles << std::endl;
  out << indent << "Bandwidth throttling : " << stats.slowdown << std::endl;
  
  // Print per-DataSpaceID stats.
  for (unsigned pvi = 0; pvi < unsigned(problem::GetShape()->NumDataSpaces); pvi++)
  {
    auto pv = problem::Shape::DataSpaceID(pvi);

    if (stats.keep.at(pv))
    {
      out << indent << problem::GetShape()->DataSpaceIDToName.at(pv) << ":" << std::endl;

// flag to print verbose sparse stats or dense stats
// #define PRINT_SPARSE_STATS
#ifdef PRINT_SPARSE_STATS
      out << indent + indent << "Partition size                                        : " << stats.partition_size.at(pv) << std::endl;
      out << indent + indent << "Parent level name                                     : " << stats.parent_level_name.at(pv) << std::endl;
      out << indent + indent << "Tile confidence                                       : " << stats.tile_confidence.at(pv) << std::endl;
      out << indent + indent << "Max tile density                                      : " << stats.tile_max_density.at(pv) << std::endl;
      out << indent + indent << "Tile size                                             : " << stats.tile_size.at(pv) << std::endl;
      out << indent + indent << "Max total utilized capacity                           : " << stats.utilized_capacity.at(pv) << std::endl;
      out << indent + indent << "Utilized instances (max)                              : " << stats.utilized_instances.at(pv) << std::endl;
      out << indent + indent << "Utilized clusters (max)                               : " << stats.utilized_clusters.at(pv) << std::endl;
      out << indent + indent << "Max metadata tile size                                : " << stats.metadata_tile_size.at(pv) << std::endl;
      out << indent + indent << "Max metadata utilized capacity                        : " << int(ceil((double)stats.metadata_tile_size.at(pv) * specs_.metadata_word_bits.Get()/specs_.word_bits.Get())) << std::endl;
      out << indent + indent << "Total scalar reads (per-instance)                     : " << stats.reads.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar skipped reads (per-instance): " << stats.skipped_reads.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar gated reads (per-instance): " << stats.gated_reads.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar random reads (per-instance): " << stats.random_reads.at(pv) << std::endl;
      out << indent + indent << "Total scalar fills (per-instance)                     : " << stats.fills.at(pv) << std::endl;
      out << indent + indent + indent << "Total skipped fills (per-instance): " << stats.skipped_fills.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar gated fills (per-instance): " << stats.gated_fills.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar random fills (per-instance): " << stats.random_fills.at(pv) << std::endl;
      out << indent + indent << "Total scalar updates (per-instance)                   : " << stats.updates.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar skipped  updates (per-instance): " << stats.skipped_updates.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar gated  updates (per-instance): " << stats.gated_updates.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar random  updates (per-instance): " << stats.random_updates.at(pv) << std::endl;
      out << indent + indent << "Temporal reductions (per-instance)                    : " << stats.temporal_reductions.at(pv) << std::endl;
      out << indent + indent << "Address generations (per-cluster)                     : " << stats.address_generations.at(pv) << std::endl;
      out << indent + indent << "Total scalar metadata reads (per-cluster)             : " << stats.metadata_reads.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata random reads (per-cluster): " << stats.random_metadata_reads.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata gated reads (per-cluster): " << stats.gated_metadata_reads.at(pv) << std::endl;
      out << indent + indent << "Total scalar metadata fills (per-cluster)             : " << stats.metadata_fills.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata random fills (per-cluster): " << stats.random_metadata_fills.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata gated fills (per-cluster): " << stats.gated_metadata_fills.at(pv) << std::endl;
      out << indent + indent << "Total scalar metadata updates (per-cluster)           : " << stats.metadata_updates.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata random updates (per-cluster): " << stats.random_metadata_updates.at(pv) << std::endl;
      out << indent + indent + indent << "Scalar metadata gated updates (per-cluster): " << stats.gated_metadata_updates.at(pv) << std::endl;
      out << indent + indent << "Scalar decompression counts (per-cluster)             : " << stats.decompression_counts.at(pv) << std::endl;
      out << indent + indent << "Scalar compression counts (per-cluster)               : " << stats.compression_counts.at(pv) << std::endl;
      out << indent + indent << "Speculation energy cost (total)                       : "  << stats.speculation_energy_cost.at(pv)* stats.utilized_instances.at(pv)<< std::endl;
      out << indent + indent << "Energy (per-scalar-access)                            : " << stats.energy_per_access.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Energy (per-instance)                                 : " << stats.energy.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Energy (total)                                        : " << stats.energy.at(pv) * stats.utilized_instances.at(pv)
          << " pJ" << std::endl;
      out << indent + indent << "Temporal Reduction Energy (per-instance)              : "
          << stats.temporal_reduction_energy.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Temporal Reduction Energy (total)                     : "
          << stats.temporal_reduction_energy.at(pv) * stats.utilized_instances.at(pv)
          << " pJ" << std::endl;
      // out << indent + indent << "Address Generation Energy (per-cluster)  : "
      //     << stats.addr_gen_energy.at(pv) << " pJ" << std::endl;
      // out << indent + indent << "Address Generation Energy (total)        : "
      //     << stats.addr_gen_energy.at(pv) * stats.utilized_clusters.at(pv)
      //     << " pJ" << std::endl;
      out << indent + indent << "Read Bandwidth (per-instance)                         : " << stats.read_bandwidth.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Read Bandwidth (total)                                : " << stats.read_bandwidth.at(pv) * stats.utilized_instances.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Write Bandwidth (per-instance)                        : " << stats.write_bandwidth.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Write Bandwidth (total)                               : " << stats.write_bandwidth.at(pv) * stats.utilized_instances.at(pv) << " words/cycle" << std::endl;

#else
      out << indent + indent << "Partition size                           : " << stats.partition_size.at(pv) << std::endl;
      out << indent + indent << "Utilized capacity                        : " << stats.utilized_capacity.at(pv) << std::endl;
      out << indent + indent << "Utilized instances (max)                 : " << stats.utilized_instances.at(pv) << std::endl;
      out << indent + indent << "Utilized clusters (max)                  : " << stats.utilized_clusters.at(pv) << std::endl;
      out << indent + indent << "Scalar reads (per-instance)              : " << stats.reads.at(pv) << std::endl;
      out << indent + indent << "Scalar updates (per-instance)            : " << stats.updates.at(pv) << std::endl;
      out << indent + indent << "Scalar fills (per-instance)              : " << stats.fills.at(pv) << std::endl;
      out << indent + indent << "Temporal reductions (per-instance)       : " << stats.temporal_reductions.at(pv) << std::endl;
      out << indent + indent << "Address generations (per-cluster)        : " << stats.address_generations.at(pv) << std::endl;

      out << indent + indent << "Energy (per-scalar-access)               : " << stats.energy_per_access.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Energy (per-instance)                    : " << stats.energy.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Energy (total)                           : " << stats.energy.at(pv) * stats.utilized_instances.at(pv)
          << " pJ" << std::endl;
      out << indent + indent << "Temporal Reduction Energy (per-instance) : "
          << stats.temporal_reduction_energy.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Temporal Reduction Energy (total)        : "
          << stats.temporal_reduction_energy.at(pv) * stats.utilized_instances.at(pv)
          << " pJ" << std::endl;
      out << indent + indent << "Address Generation Energy (per-cluster)  : "
          << stats.addr_gen_energy.at(pv) << " pJ" << std::endl;
      out << indent + indent << "Address Generation Energy (total)        : "
          << stats.addr_gen_energy.at(pv) * stats.utilized_clusters.at(pv)
          << " pJ" << std::endl;
      out << indent + indent << "Read Bandwidth (per-instance)            : " << stats.read_bandwidth.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Read Bandwidth (total)                   : " << stats.read_bandwidth.at(pv) * stats.utilized_instances.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Write Bandwidth (per-instance)           : " << stats.write_bandwidth.at(pv) << " words/cycle" << std::endl;
      out << indent + indent << "Write Bandwidth (total)                  : " << stats.write_bandwidth.at(pv) * stats.utilized_instances.at(pv) << " words/cycle" << std::endl;
#endif
    }
  }

  out << std::endl;
}

}  // namespace model
