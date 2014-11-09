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
#include <functional>
#include <vector>
#include <iostream>
#include <algorithm>
#include "fruit/impl/util/demangle_type_name.h"
#include "fruit/impl/util/type_info.h"

#include "fruit/impl/storage/normalized_component_storage.h"
#include "fruit/impl/storage/component_storage.h"

#include "fruit/impl/data_structures/semistatic_map.templates.h"
#include "fruit/impl/data_structures/semistatic_graph.templates.h"

using std::cout;
using std::endl;

using namespace fruit;
using namespace fruit::impl;

namespace {

auto typeInfoLessThanForMultibindings = [](const std::pair<TypeId, MultibindingData>& x,
                                           const std::pair<TypeId, MultibindingData>& y) {
  return x.first < y.first;
};

// Used to construct the SemistaticGraph below, wrapping a std::vector<std::pair<TypeId, BindingData>>::iterator.
struct BindingDataNodeIter {
  std::vector<std::pair<TypeId, BindingData>>::iterator itr;
  
  BindingDataNodeIter* operator->() {
    return this;
  }
  
  void operator++() {
    ++itr;
  }
  
  bool operator==(const BindingDataNodeIter& other) const {
    return itr == other.itr;
  }
  
  bool operator!=(const BindingDataNodeIter& other) const {
    return itr != other.itr;
  }
  
  TypeId getId() {
    return itr->first;
  }
  
  NormalizedBindingData getValue() {
    BindingData& bindingData = itr->second;
    if (bindingData.isCreated()) {
      return NormalizedBindingData{bindingData.getStoredSingleton()};
    } else {
      return NormalizedBindingData{bindingData.getCreate()};
    }
  }
  
  bool isTerminal() {
    return itr->second.isCreated();
  }
  
  const TypeId* getEdgesBegin() {
    const BindingDeps* deps = itr->second.getDeps();
    return deps->deps;
  }
  
  const TypeId* getEdgesEnd() {
    const BindingDeps* deps = itr->second.getDeps();
    return deps->deps + deps->num_deps;
  }
};

} // namespace

namespace fruit {
namespace impl {

std::string NormalizedComponentStorage::multipleBindingsError(TypeId typeId) {
  return "Fatal injection error: the type " + typeId.type_info->name() + " was provided more than once, with different bindings.\n"
        + "This was not caught at compile time because at least one of the involved components bound this type but didn't expose it in the component signature.\n"
        + "If the type has a default constructor or an Inject annotation, this problem may arise even if this type is bound/provided by only one component (and then hidden), if this type is auto-injected in another component.\n"
        + "If the source of the problem is unclear, try exposing this type in all the component signatures where it's bound; if no component hides it this can't happen.\n";
}

NormalizedComponentStorage::NormalizedComponentStorage(BindingVectors&& bindingVectors) {
  
  std::vector<std::pair<TypeId, BindingData>>& typeRegistryVector = bindingVectors.first;
  std::vector<std::pair<TypeId, MultibindingData>>& typeRegistryVectorForMultibindings = bindingVectors.second;
  
  std::sort(typeRegistryVector.begin(), typeRegistryVector.end());
  
  // Now duplicates (either consistent or non-consistent) might exist.
  auto firstFreePos = typeRegistryVector.begin();
  for (auto i = typeRegistryVector.begin(); i != typeRegistryVector.end(); /* no increment */) {
    TypeId typeId = i->first;
    BindingData& x = i->second;
    *firstFreePos = *i;
    ++firstFreePos;
    
    // Check that other bindings for the same type (if any) are equal.
    for (++i; i != typeRegistryVector.end() && i->first == typeId; ++i) {
      if (!(x == i->second)) {
        std::cerr << multipleBindingsError(typeId) << std::endl;
        exit(1);
      }
    }
  }
  typeRegistryVector.erase(firstFreePos, typeRegistryVector.end());
  
  for (const auto& p : typeRegistryVector) {
    total_size += InjectorStorage::maximumRequiredSpace(p.first);
  }
  
  typeRegistry = SemistaticGraph<TypeId, NormalizedBindingData>(BindingDataNodeIter{typeRegistryVector.begin()},
                                                                BindingDataNodeIter{typeRegistryVector.end()});
  
  std::sort(typeRegistryVectorForMultibindings.begin(), typeRegistryVectorForMultibindings.end(), typeInfoLessThanForMultibindings);
  
  // Now we must merge multiple bindings for the same type.
  for (auto i = typeRegistryVectorForMultibindings.begin(); i != typeRegistryVectorForMultibindings.end(); /* no increment */) {
    std::pair<TypeId, MultibindingData>& x = *i;
    
    NormalizedMultibindingData& b = typeRegistryForMultibindings[x.first];
    b.getSingletonsVector = x.second.getSingletonsVector;
    
    // Insert all multibindings for this type (note that x is also inserted here).
    for (; i != typeRegistryVectorForMultibindings.end() && i->first == x.first; ++i) {
      b.elems.push_back(NormalizedMultibindingData::Elem(i->second));
    }
  }
  
  for (const auto& typeInfoDataPair : typeRegistryForMultibindings) {
    total_size += InjectorStorage::maximumRequiredSpace(typeInfoDataPair.first) * typeInfoDataPair.second.elems.size();
  }
}

// TODO: This can't be inline (let alone defined as `=default') with GCC 4.8, while it would work anyway with Clang.
// Consider minimizing the testcase and filing a bug.
NormalizedComponentStorage::~NormalizedComponentStorage() {
}

} // namespace impl
} // namespace fruit
