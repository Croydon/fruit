/*
 * Copyright 2014 Google Inc. All rights reserved.
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

#define IN_FRUIT_CPP_FILE

#include <cstdlib>
#include <memory>
#include <vector>
#include <iostream>
#include <algorithm>
#include <fruit/impl/util/type_info.h>

#include <fruit/impl/injector/injector_storage.h>
#include <fruit/impl/data_structures/semistatic_graph.templates.h>
#include <fruit/impl/normalized_component_storage/normalized_component_storage.h>
#include <fruit/impl/normalized_component_storage/binding_normalization.h>

using std::cout;
using std::endl;

using namespace fruit::impl;

using LazyComponentWithNoArgs = ComponentStorageEntry::LazyComponentWithNoArgs;
using LazyComponentWithArgs = ComponentStorageEntry::LazyComponentWithArgs;

namespace {

std::string multipleBindingsError(TypeId type) {
  return "Fatal injection error: the type " + type.type_info->name() + " was provided more than once, with different bindings.\n"
        + "This was not caught at compile time because at least one of the involved components bound this type but didn't expose it in the component signature.\n"
        + "If the type has a default constructor or an Inject annotation, this problem may arise even if this type is bound/provided by only one component (and then hidden), if this type is auto-injected in another component.\n"
        + "If the source of the problem is unclear, try exposing this type in all the component signatures where it's bound; if no component hides it this can't happen.\n";
}

void printLazyComponentInstallationLoop(TypeId toplevel_component_fun_type_id,
                                        const std::vector<ComponentStorageEntry>& entries_to_process,
                                        const ComponentStorageEntry& last_entry) {
  std::cerr << "Found a loop while expanding components passed to PartialComponent::install()." << std::endl;
  std::cerr << "Component installation trace (from top-level to the most deeply-nested):" << std::endl;
  std::cerr << std::string(toplevel_component_fun_type_id) << std::endl;
  for (const ComponentStorageEntry& entry : entries_to_process) {
    switch (entry.kind) {
    case ComponentStorageEntry::Kind::COMPONENT_WITH_ARGS_END_MARKER:
      if (entry.type_id == last_entry.type_id
          && last_entry.kind == ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_ARGS
          && *entry.lazy_component_with_args.component == *last_entry.lazy_component_with_args.component) {
        std::cerr << "<-- The loop starts here" << std::endl;
      }
      std::cerr << std::string(entry.lazy_component_with_args.component->getFunTypeId()) << std::endl;
      break;

    case ComponentStorageEntry::Kind::COMPONENT_WITHOUT_ARGS_END_MARKER:
      if (entry.type_id == last_entry.type_id
          && last_entry.kind == ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_NO_ARGS
          && entry.lazy_component_with_no_args.erased_fun == last_entry.lazy_component_with_no_args.erased_fun) {
        std::cerr << "<-- The loop starts here" << std::endl;
      }
      std::cerr << std::string(entry.type_id) << std::endl;
      break;

    default:
      break;
    }
  }

  switch (last_entry.kind) {
  case ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_ARGS:
    std::cerr << std::string(last_entry.lazy_component_with_args.component->getFunTypeId()) << std::endl;
    break;

  case ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_NO_ARGS:
    std::cerr << std::string(last_entry.type_id) << std::endl;
    break;

  default:
    break;
  }
}

auto createLazyComponentWithNoArgsSet = []() {
  return createHashSetWithCustomFunctors<LazyComponentWithNoArgs>(
      [](const LazyComponentWithNoArgs& x) {
        return x.hashCode();
      },
      [](const LazyComponentWithNoArgs& x, const LazyComponentWithNoArgs& y) {
        return x == y;
      });
};

auto createLazyComponentWithArgsSet = []() {
  return createHashSetWithCustomFunctors<LazyComponentWithArgs>(
      [](const LazyComponentWithArgs& x) {
        return x.component->hashCode();
      },
      [](const LazyComponentWithArgs& x, const LazyComponentWithArgs& y) {
        return *x.component == *y.component;
      });
};

} // namespace

namespace fruit {
namespace impl {

template <typename HandleCompressedBinding, typename HandleMultibinding>
void BindingNormalization::normalizeBindingsHelper(
    FixedSizeVector<ComponentStorageEntry>&& toplevel_entries,
    FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
    TypeId toplevel_component_fun_type_id,
    HashMap<TypeId, ComponentStorageEntry>& binding_data_map,
    HandleCompressedBinding handle_compressed_binding,
    HandleMultibinding handle_multibinding) {

  binding_data_map = createHashMap<TypeId, ComponentStorageEntry>();

  std::vector<ComponentStorageEntry> expanded_entries_vector;

  // These sets contain the lazy components whose expansion has already completed.
  auto fully_expanded_components_with_no_args = createLazyComponentWithNoArgsSet();
  auto fully_expanded_components_with_args = createLazyComponentWithArgsSet();

  // These sets contain the elements with kind *_END_MARKER in entries_to_process.
  // For component with args, these sets do *not* own the objects, entries_to_process does.
  auto components_with_no_args_with_expansion_in_progress = createLazyComponentWithNoArgsSet();
  auto components_with_args_with_expansion_in_progress = createLazyComponentWithArgsSet();

  std::vector<ComponentStorageEntry> entries_to_process(toplevel_entries.begin(), toplevel_entries.end());
  toplevel_entries.clear();

  // When we expand a lazy component, instead of removing it from the stack we change its kind (in entries_to_process)
  // to one of the *_END_MARKER kinds. This allows to keep track of the "call stack" for the expansion.

  while (!entries_to_process.empty()) {
    ComponentStorageEntry entry = entries_to_process.back();

    switch (entry.kind) {
    case ComponentStorageEntry::Kind::BINDING_FOR_CONSTRUCTED_OBJECT:
      {
        entries_to_process.pop_back();
        ComponentStorageEntry& entry_in_map = binding_data_map[entry.type_id];
        if (entry_in_map.type_id.type_info != nullptr) {
          if (entry_in_map.kind != ComponentStorageEntry::Kind::BINDING_FOR_CONSTRUCTED_OBJECT
              || entry.binding_for_constructed_object.object_ptr
                  != entry_in_map.binding_for_constructed_object.object_ptr) {
            std::cerr << multipleBindingsError(entry.type_id) << std::endl;
            exit(1);
          }
          // Otherwise ok, duplicate but consistent binding.
        } else {
          // New binding, add it to the map.
          entry_in_map = std::move(entry);
        }
      }
      break;

    case ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION:
      {
        entries_to_process.pop_back();
        ComponentStorageEntry& entry_in_map = binding_data_map[entry.type_id];
        fixed_size_allocator_data.addType(entry.type_id);
        if (entry_in_map.type_id.type_info != nullptr) {
          if (entry_in_map.kind != ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION
              || entry.binding_for_object_to_construct.create
                  != entry_in_map.binding_for_object_to_construct.create) {
            std::cerr << multipleBindingsError(entry.type_id) << std::endl;
            exit(1);
          }
          // Otherwise ok, duplicate but consistent binding.
        } else {
          // New binding, add it to the map.
          entry_in_map = std::move(entry);
        }
      }
      break;

    case ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION:
      {
        entries_to_process.pop_back();
        ComponentStorageEntry& entry_in_map = binding_data_map[entry.type_id];
        fixed_size_allocator_data.addExternallyAllocatedType(entry.type_id);
        if (entry_in_map.type_id.type_info != nullptr) {
          if (entry_in_map.kind != ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION
              || entry.binding_for_object_to_construct.create
                  != entry_in_map.binding_for_object_to_construct.create) {
            std::cerr << multipleBindingsError(entry.type_id) << std::endl;
            exit(1);
          }
          // Otherwise ok, duplicate but consistent binding.
        } else {
          // New binding, add it to the map.
          entry_in_map = std::move(entry);
        }
      }
      break;

    case ComponentStorageEntry::Kind::COMPRESSED_BINDING:
      {
        entries_to_process.pop_back();
        handle_compressed_binding(entry);
      }
      break;


    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT:
    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION:
    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION:
      {
        entries_to_process.pop_back();
        FruitAssert(!entries_to_process.empty());
        ComponentStorageEntry vector_creator_entry = std::move(entries_to_process.back());
        entries_to_process.pop_back();
        FruitAssert(vector_creator_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_VECTOR_CREATOR);
        handle_multibinding(entry, vector_creator_entry);
      }
      break;

    case ComponentStorageEntry::Kind::MULTIBINDING_VECTOR_CREATOR:
      {
        entries_to_process.pop_back();
        FruitAssert(!entries_to_process.empty());
        ComponentStorageEntry multibinding_entry = std::move(entries_to_process.back());
        entries_to_process.pop_back();
        FruitAssert(
            multibinding_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT
                || multibinding_entry.kind
                    == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION
                || multibinding_entry.kind
                    == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION);
        handle_multibinding(multibinding_entry, entry);
      }
      break;

    case ComponentStorageEntry::Kind::COMPONENT_WITHOUT_ARGS_END_MARKER:
      {
        entries_to_process.pop_back();
        // A lazy component expansion has completed; we now move the component from
        // components_with_*_with_expansion_in_progress to fully_expanded_components_*.

        components_with_no_args_with_expansion_in_progress.erase(entry.lazy_component_with_no_args);
        fully_expanded_components_with_no_args.insert(std::move(entry.lazy_component_with_no_args));
      }
      break;

    case ComponentStorageEntry::Kind::COMPONENT_WITH_ARGS_END_MARKER:
      {
        entries_to_process.pop_back();
        // A lazy component expansion has completed; we now move the component from
        // components_with_*_with_expansion_in_progress to fully_expanded_components_*.

        components_with_args_with_expansion_in_progress.erase(entry.lazy_component_with_args);
        fully_expanded_components_with_args.insert(std::move(entry.lazy_component_with_args));
      }
      break;

    case ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_ARGS:
      {
        if (fully_expanded_components_with_args.count(entry.lazy_component_with_args)) {
          // This lazy component was already inserted, skip it.
          entries_to_process.pop_back();
          continue;
        }

        bool actually_inserted =
            components_with_args_with_expansion_in_progress.insert(entry.lazy_component_with_args).second;
        if (!actually_inserted) {
          printLazyComponentInstallationLoop(
              toplevel_component_fun_type_id, entries_to_process, entry);
          exit(1);
        }

#ifdef FRUIT_EXTRA_DEBUG
        std::cout << "Expanding lazy component: " << entry.lazy_component_with_args.component->getFunTypeId() << std::endl;
#endif

        // Instead of removing the component from component.lazy_components, we just change its kind to the
        // corresponding *_END_MARKER kind.
        // When we pop this marker, this component's expansion will be complete.
        entries_to_process.back().kind = ComponentStorageEntry::Kind::COMPONENT_WITH_ARGS_END_MARKER;

        // Note that this can also add other lazy components, so the resulting bindings can have a non-intuitive
        // (although deterministic) order.
        entries_to_process.back().lazy_component_with_args.component->addBindings(entries_to_process);

        break;
      }

    case ComponentStorageEntry::Kind::LAZY_COMPONENT_WITH_NO_ARGS:
      {
        if (fully_expanded_components_with_no_args.count(entry.lazy_component_with_no_args)) {
          // This lazy component was already inserted, skip it.
          entries_to_process.pop_back();
          continue;
        }

        bool actually_inserted =
            components_with_no_args_with_expansion_in_progress.insert(entry.lazy_component_with_no_args).second;
        if (!actually_inserted) {
          printLazyComponentInstallationLoop(
              toplevel_component_fun_type_id, entries_to_process, entry);
          exit(1);
        }

    #ifdef FRUIT_EXTRA_DEBUG
        std::cout << "Expanding lazy component: " << entry.type_id << std::endl;
    #endif

        // Instead of removing the component from component.lazy_components, we just change its kind to the
        // corresponding *_END_MARKER kind.
        // When we pop this marker, this component's expansion will be complete.
        entries_to_process.back().kind = ComponentStorageEntry::Kind::COMPONENT_WITHOUT_ARGS_END_MARKER;

        // Note that this can also add other lazy components, so the resulting bindings can have a non-intuitive
        // (although deterministic) order.
        entries_to_process.back().lazy_component_with_no_args.addBindings(entries_to_process);

        break;
      }

    default:
#ifdef FRUIT_EXTRA_DEBUG
      std::cerr << "Unexpected kind: " << (std::size_t)entries_to_process.back().kind << std::endl;
#endif
      FruitAssert(false);
    }
  }

  FruitAssert(components_with_no_args_with_expansion_in_progress.empty());
  FruitAssert(components_with_args_with_expansion_in_progress.empty());
}

void BindingNormalization::normalizeBindings(
    FixedSizeVector<ComponentStorageEntry>&& toplevel_entries,
    FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
    TypeId toplevel_component_fun_type_id,
    const std::vector<TypeId>& exposed_types,
    std::vector<ComponentStorageEntry>& bindings_vector,
    std::vector<std::pair<ComponentStorageEntry, ComponentStorageEntry>>& multibindings_vector,
    BindingCompressionInfoMap& bindingCompressionInfoMap) {

  HashMap<TypeId, ComponentStorageEntry> binding_data_map;
  HashMap<TypeId, BindingNormalization::BindingCompressionInfo> compressed_bindings_map;

  // CtypeId -> (ItypeId, bindingData)
  compressed_bindings_map = createHashMap<TypeId, BindingCompressionInfo>();
  multibindings_vector.clear();

  normalizeBindingsHelper(
      std::move(toplevel_entries),
      fixed_size_allocator_data,
      toplevel_component_fun_type_id,
      binding_data_map,
      [&compressed_bindings_map](ComponentStorageEntry entry) {
        BindingCompressionInfo& compression_info = compressed_bindings_map[entry.compressed_binding.c_type_id];
        compression_info.i_type_id = entry.type_id;
        compression_info.create_i_with_compression = entry.compressed_binding.create;
      },
      [&multibindings_vector](ComponentStorageEntry multibinding,
                              ComponentStorageEntry multibinding_vector_creator) {
        multibindings_vector.emplace_back(multibinding, multibinding_vector_creator);
      });

  bindings_vector =
      BindingNormalization::performBindingCompression(
          std::move(binding_data_map),
          std::move(compressed_bindings_map),
          multibindings_vector,
          exposed_types,
          bindingCompressionInfoMap);
}

void BindingNormalization::normalizeBindingsWithoutBindingCompression(
    FixedSizeVector<ComponentStorageEntry>&& toplevel_entries,
    FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
    TypeId toplevel_component_fun_type_id,
    std::vector<ComponentStorageEntry>& bindings_vector,
    std::vector<std::pair<ComponentStorageEntry, ComponentStorageEntry>>& multibindings_vector) {

  HashMap<TypeId, ComponentStorageEntry> binding_data_map;
  multibindings_vector.clear();

  normalizeBindingsHelper(
      std::move(toplevel_entries),
      fixed_size_allocator_data,
      toplevel_component_fun_type_id,
      binding_data_map,
      [](ComponentStorageEntry) {},
      [&multibindings_vector](ComponentStorageEntry multibinding,
                              ComponentStorageEntry multibinding_vector_creator) {
        multibindings_vector.emplace_back(multibinding, multibinding_vector_creator);
      });

  // Copy the normalized bindings into the result vector.
  bindings_vector.clear();
  bindings_vector.reserve(binding_data_map.size());
  for (auto& p : binding_data_map) {
    bindings_vector.push_back(p.second);
  }
}

std::vector<ComponentStorageEntry> BindingNormalization::performBindingCompression(
    HashMap<TypeId, ComponentStorageEntry> &&binding_data_map,
    HashMap<TypeId, BindingCompressionInfo> &&compressed_bindings_map,
    const std::vector<std::pair<ComponentStorageEntry, ComponentStorageEntry>> &multibindings_vector,
    const std::vector<TypeId> &exposed_types,
    BindingCompressionInfoMap &bindingCompressionInfoMap) {
  std::vector<ComponentStorageEntry> result;

  // We can't compress the binding if C is a dep of a multibinding.
  for (const std::pair<ComponentStorageEntry, ComponentStorageEntry>& multibinding_entry_pair : multibindings_vector) {
    const ComponentStorageEntry& entry = multibinding_entry_pair.first;
    FruitAssert(entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT
        || entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION
        || entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION);
    if (entry.kind != ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT) {
      const BindingDeps* deps = entry.multibinding_for_object_to_construct.deps;
      FruitAssert(deps != nullptr);
      for (std::size_t i = 0; i < deps->num_deps; ++i) {
        compressed_bindings_map.erase(deps->deps[i]);
#ifdef FRUIT_EXTRA_DEBUG
        std::cout << "InjectorStorage: ignoring compressed binding for " << deps->deps[i] << " because it's a dep of a multibinding." << std::endl;
#endif
      }
    }
  }

  // We can't compress the binding if C is an exposed type (but I is likely to be exposed instead).
  for (TypeId type : exposed_types) {
    compressed_bindings_map.erase(type);
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << "InjectorStorage: ignoring compressed binding for " << type << " because it's an exposed type." << std::endl;
#endif
  }

  // We can't compress the binding if some type X depends on C and X!=I.
  for (auto& binding_data_map_entry : binding_data_map) {
    TypeId x_id = binding_data_map_entry.first;
    ComponentStorageEntry entry = binding_data_map_entry.second;
    FruitAssert(entry.kind == ComponentStorageEntry::Kind::BINDING_FOR_CONSTRUCTED_OBJECT
        || entry.kind == ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION
        || entry.kind == ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION);

    if (entry.kind != ComponentStorageEntry::Kind::BINDING_FOR_CONSTRUCTED_OBJECT) {
      for (std::size_t i = 0; i < entry.binding_for_object_to_construct.deps->num_deps; ++i) {
        TypeId c_id = entry.binding_for_object_to_construct.deps->deps[i];
        auto itr = compressed_bindings_map.find(c_id);
        if (itr != compressed_bindings_map.end() && itr->second.i_type_id != x_id) {
          compressed_bindings_map.erase(itr);
#ifdef FRUIT_EXTRA_DEBUG
          std::cout << "InjectorStorage: ignoring compressed binding for " << c_id << " because the type " <<  x_id << " depends on it." << std::endl;
#endif
        }
      }
    }
  }

  // Two pairs of compressible bindings (I->C) and (C->X) can not exist (the C of a compressible binding is always bound either
  // using constructor binding or provider binding, it can't be a binding itself). So no need to check for that.

  bindingCompressionInfoMap =
      createHashMap<TypeId, NormalizedComponentStorage::CompressedBindingUndoInfo>(compressed_bindings_map.size());

  // Now perform the binding compression.
  for (auto& entry : compressed_bindings_map) {
    TypeId c_id = entry.first;
    TypeId i_id = entry.second.i_type_id;
    auto i_binding_data = binding_data_map.find(i_id);
    auto c_binding_data = binding_data_map.find(c_id);
    FruitAssert(i_binding_data != binding_data_map.end());
    FruitAssert(c_binding_data != binding_data_map.end());
    NormalizedComponentStorage::CompressedBindingUndoInfo& undo_info = bindingCompressionInfoMap[c_id];
    undo_info.i_type_id = i_id;
    FruitAssert(i_binding_data->second.kind == ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION);
    undo_info.i_binding = i_binding_data->second.binding_for_object_to_construct;
    FruitAssert(
        c_binding_data->second.kind == ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION
        || c_binding_data->second.kind == ComponentStorageEntry::Kind::BINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION);
    undo_info.c_binding = c_binding_data->second.binding_for_object_to_construct;
    // Note that even if I is the one that remains, C is the one that will be allocated, not I.

    i_binding_data->second.kind = c_binding_data->second.kind;
    i_binding_data->second.binding_for_object_to_construct.create = entry.second.create_i_with_compression;
    i_binding_data->second.binding_for_object_to_construct.deps =
        c_binding_data->second.binding_for_object_to_construct.deps;
    binding_data_map.erase(c_binding_data);
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << "InjectorStorage: performing binding compression for the edge " << i_id << "->" << c_id << std::endl;
#endif
  }

  // Copy the normalized bindings into the result vector.
  result.reserve(binding_data_map.size());
  for (auto& p : binding_data_map) {
    result.push_back(p.second);
  }

  return result;
}

void BindingNormalization::addMultibindings(std::unordered_map<TypeId, NormalizedMultibindingSet>&
                                                multibindings,
                                            FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
                                            std::vector<std::pair<ComponentStorageEntry, ComponentStorageEntry>>&& multibindingsVector) {

#ifdef FRUIT_EXTRA_DEBUG
  std::cout << "InjectorStorage: adding multibindings:" << std::endl;
#endif
  // Now we must merge multiple bindings for the same type.
  for (auto i = multibindingsVector.begin(); i != multibindingsVector.end(); ++i) {
    const ComponentStorageEntry& multibinding_entry = i->first;
    const ComponentStorageEntry& multibinding_vector_creator_entry = i->second;
    FruitAssert(
        multibinding_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION
        || multibinding_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION
        || multibinding_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT);
    FruitAssert(multibinding_vector_creator_entry.kind == ComponentStorageEntry::Kind::MULTIBINDING_VECTOR_CREATOR);
    NormalizedMultibindingSet& b = multibindings[multibinding_entry.type_id];

    // Might be set already, but we need to set it if there was no multibinding for this type.
    b.get_multibindings_vector = multibinding_vector_creator_entry.multibinding_vector_creator.get_multibindings_vector;

    switch (i->first.kind) {
    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_CONSTRUCTED_OBJECT:
      {
        NormalizedMultibinding normalized_multibinding;
        normalized_multibinding.is_constructed = true;
        normalized_multibinding.object = i->first.multibinding_for_constructed_object.object_ptr;
        b.elems.push_back(std::move(normalized_multibinding));
      }
      break;

    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_NO_ALLOCATION:
      {
        fixed_size_allocator_data.addExternallyAllocatedType(i->first.type_id);
        NormalizedMultibinding normalized_multibinding;
        normalized_multibinding.is_constructed = false;
        normalized_multibinding.create = i->first.multibinding_for_object_to_construct.create;
        b.elems.push_back(std::move(normalized_multibinding));
      }
      break;

    case ComponentStorageEntry::Kind::MULTIBINDING_FOR_OBJECT_TO_CONSTRUCT_THAT_NEEDS_ALLOCATION:
      {
        fixed_size_allocator_data.addType(i->first.type_id);
        NormalizedMultibinding normalized_multibinding;
        normalized_multibinding.is_constructed = false;
        normalized_multibinding.create = i->first.multibinding_for_object_to_construct.create;
        b.elems.push_back(std::move(normalized_multibinding));
      }
      break;

    default:
#ifdef FRUIT_EXTRA_DEBUG
      std::cerr << "Unexpected kind: " << (std::size_t)i->first.kind << std::endl;
#endif
      FruitAssert(false);
    }
  }
}

} // namespace impl
} // namespace fruit
