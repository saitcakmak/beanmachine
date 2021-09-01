// Copyright 2004-present Facebook. All Rights Reserved.

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "beanmachine/graph/distribution/distribution.h"
#include "beanmachine/graph/global/global_state.h"
#include "beanmachine/graph/operator/operator.h"
#include "beanmachine/graph/operator/stochasticop.h"
#include "beanmachine/graph/util.h"

namespace beanmachine {
namespace graph {

GlobalState::GlobalState(Graph& g, uint seed) : graph(g) {
  std::mt19937 gen(seed);
  flat_size = 0;
  std::set<uint> supp = graph.compute_support();
  for (uint node_id : supp) {
    ordered_support.push_back(graph.nodes[node_id].get());
  }

  // initialize values
  for (auto node : ordered_support) {
    if (!node->is_observed) {
      // TODO: add different methods of initialization
      node->eval(gen);
    }
    if (node->is_stochastic() and node->node_type == NodeType::OPERATOR) {
      auto sto_node = static_cast<oper::StochasticOperator*>(node);
      sto_node->get_unconstrained_value(true);
    }
  }

  // update backward gradients
  update_backgrad();

  // save stochastic and deterministic nodes
  for (auto node : ordered_support) {
    if (node->is_stochastic() and !node->is_observed) {
      stochastic_nodes.push_back(node);
      // initialize vals_backup and grads_backup to correct size
      auto stochastic_node = static_cast<oper::StochasticOperator*>(node);
      NodeValue unconstrained_value =
          *stochastic_node->get_unconstrained_value(false);
      stochastic_unconstrained_vals_backup.push_back(unconstrained_value);
      stochastic_unconstrained_grads_backup.push_back(
          stochastic_node->back_grad1);
    } else if (!node->is_stochastic()) {
      deterministic_nodes.push_back(node);
    }
  }

  // calculate total size of unobserved unconstrained stochastic values
  for (Node* node : stochastic_nodes) {
    auto stochastic_node = static_cast<oper::StochasticOperator*>(node);
    NodeValue unconstrained_value =
        *stochastic_node->get_unconstrained_value(false);
    if (unconstrained_value.type.variable_type == VariableType::SCALAR) {
      flat_size++;
    } else {
      flat_size += unconstrained_value._matrix.size();
    }
  }

  backup_unconstrained_values();
  backup_unconstrained_grads();
  update_log_prob();
}

void GlobalState::backup_unconstrained_values() {
  for (uint sto_node_id = 0; sto_node_id < stochastic_nodes.size();
       sto_node_id++) {
    auto stochastic_node =
        static_cast<oper::StochasticOperator*>(stochastic_nodes[sto_node_id]);
    stochastic_unconstrained_vals_backup[sto_node_id] =
        *stochastic_node->get_unconstrained_value(false);
  }
}

void GlobalState::backup_unconstrained_grads() {
  for (uint sto_node_id = 0; sto_node_id < stochastic_nodes.size();
       sto_node_id++) {
    stochastic_unconstrained_grads_backup[sto_node_id] =
        stochastic_nodes[sto_node_id]->back_grad1;
  }
}

void GlobalState::revert_unconstrained_values() {
  for (uint sto_node_id = 0; sto_node_id < stochastic_nodes.size();
       sto_node_id++) {
    auto stochastic_node =
        static_cast<oper::StochasticOperator*>(stochastic_nodes[sto_node_id]);
    NodeValue* value = stochastic_node->get_unconstrained_value(false);
    *value = stochastic_unconstrained_vals_backup[sto_node_id];
    stochastic_node->get_original_value(true);
  }
}

void GlobalState::revert_unconstrained_grads() {
  for (uint sto_node_id = 0; sto_node_id < stochastic_nodes.size();
       sto_node_id++) {
    stochastic_nodes[sto_node_id]->back_grad1 =
        stochastic_unconstrained_grads_backup[sto_node_id];
  }
}

void GlobalState::add_to_stochastic_unconstrained_nodes(
    Eigen::VectorXd& increment) {
  if (increment.size() != flat_size) {
    throw std::invalid_argument(
        "The size of increment is inconsistent with the values in the graph");
  }
  Eigen::VectorXd flattened_values;
  get_flattened_unconstrained_values(flattened_values);
  Eigen::VectorXd sum = flattened_values + increment;
  set_flattened_unconstrained_values(sum);
}

void GlobalState::get_flattened_unconstrained_values(
    Eigen::VectorXd& flattened_values) {
  flattened_values.resize(flat_size);
  int i = 0;
  for (Node* node : stochastic_nodes) {
    auto sto_node = static_cast<oper::StochasticOperator*>(node);
    NodeValue* value = sto_node->get_unconstrained_value(false);
    if (value->type.variable_type == VariableType::SCALAR) {
      flattened_values[i] = value->_double;
      i++;
    } else {
      Eigen::VectorXd vector(Eigen::Map<Eigen::VectorXd>(
          value->_matrix.data(), value->_matrix.size()));
      flattened_values.segment(i, vector.size()) = vector;
      i += value->_matrix.size();
    }
  }
}

void GlobalState::set_flattened_unconstrained_values(
    Eigen::VectorXd& flattened_values) {
  if (flattened_values.size() != flat_size) {
    throw std::invalid_argument(
        "The size of flattened_values is inconsistent with the values in the graph");
  }

  int i = 0;
  for (Node* node : stochastic_nodes) {
    // set unconstrained value
    auto sto_node = static_cast<oper::StochasticOperator*>(node);
    NodeValue* value = sto_node->get_unconstrained_value(false);
    if (value->type.variable_type == VariableType::SCALAR) {
      value->_double = flattened_values[i];
      i++;
    } else {
      value->_matrix = flattened_values.segment(i, value->_matrix.size());
      i += value->_matrix.size();
    }

    // sync value with unconstrained_value
    if (sto_node->transform_type != TransformType::NONE) {
      sto_node->get_original_value(true);
    }
  }
}

void GlobalState::get_flattened_unconstrained_grads(
    Eigen::VectorXd& flattened_grad) {
  flattened_grad.resize(flat_size);
  int i = 0;
  for (Node* node : stochastic_nodes) {
    if (node->value.type.variable_type == VariableType::SCALAR) {
      flattened_grad[i] = node->back_grad1._double;
      i++;
    } else {
      Eigen::VectorXd vector(Eigen::Map<Eigen::VectorXd>(
          node->back_grad1._matrix.data(), node->back_grad1._matrix.size()));
      flattened_grad.segment(i, vector.size()) = vector;
      i += node->back_grad1._matrix.size();
    }
  }
}

double GlobalState::get_log_prob() {
  return log_prob;
}

void GlobalState::update_log_prob() {
  log_prob = graph._full_log_prob(ordered_support);
}

void GlobalState::update_backgrad() {
  graph.update_backgrad(ordered_support);
}

} // namespace graph
} // namespace beanmachine