// Copyright (c) 2014 Dropbox, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdio>
#include <cstdlib>

#include <unordered_set>
#include <deque>

#include "core/common.h"

#include "core/ast.h"
#include "core/cfg.h"
#include "core/util.h"

#include "analysis/fpc.h"
#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"

namespace pyston {

class LivenessBBVisitor : public NoopASTVisitor {
    public:
        typedef std::unordered_set<std::string> StrSet;
    private:
        StrSet _loads;
        StrSet _stores;

        void _doLoad(const std::string& name) {
            if (_stores.count(name))
                return;
            _loads.insert(name);
        }
        void _doStore(const std::string& name) {
            if (_loads.count(name))
                return;
            _stores.insert(name);
        }
    public:
        LivenessBBVisitor() {
        }
        const StrSet& loads() { return _loads; }
        const StrSet& stores() { return _stores; }

        bool visit_classdef(AST_ClassDef* node) {
            _doStore(node->name);
            return true;
        }
        bool visit_functiondef(AST_FunctionDef* node) {
            _doStore(node->name);
            return true;
        }
        bool visit_name(AST_Name* node) {
            if (node->ctx_type == AST_TYPE::Load)
                _doLoad(node->id);
            else if (node->ctx_type == AST_TYPE::Store)
                _doStore(node->id);
            else {
                assert(0);
                abort();
            }
            return true;
        }
};

bool LivenessAnalysis::isLiveAtEnd(const std::string &name, CFGBlock *block) {
    if (block->successors.size() == 0)
        return false;

    // Very inefficient liveness analysis:
    // for each query, trace forward through all possible control flow paths.
    // if we hit a store to the name, stop tracing that path
    // if we hit a load to the name, return true.
    std::unordered_set<CFGBlock*> visited;
    std::deque<CFGBlock*> q;
    for (int i = 0; i < block->successors.size(); i++) {
        q.push_back(block->successors[i]);
    }

    while(q.size()) {
        CFGBlock* thisblock = q.front();
        q.pop_front();
        if (visited.count(thisblock))
            continue;
        visited.insert(thisblock);

        LivenessBBVisitor visitor;
        for (int i = 0; i < thisblock->body.size(); i++) {
            thisblock->body[i]->accept(&visitor);
        }
        if (visitor.loads().count(name))
            return true;
        if (!visitor.stores().count(name)) {
            for (int i = 0; i < thisblock->successors.size(); i++) {
                q.push_back(thisblock->successors[i]);
            }
        }
    }

    return false;
}

class DefinednessBBAnalyzer : public BBAnalyzer<DefinednessAnalysis::DefinitionLevel> {
    private:
        typedef DefinednessAnalysis::DefinitionLevel DefinitionLevel;

        AST_arguments* arguments;
    public:
        DefinednessBBAnalyzer(AST_arguments* arguments) : arguments(arguments) {
        }

        virtual DefinitionLevel merge(DefinitionLevel from, DefinitionLevel into) const {
            assert(from != DefinednessAnalysis::Undefined);
            assert(into != DefinednessAnalysis::Undefined);
            if (from == DefinednessAnalysis::PotentiallyDefined || into == DefinednessAnalysis::PotentiallyDefined)
                return DefinednessAnalysis::PotentiallyDefined;
            return DefinednessAnalysis::Defined;
        }
        virtual void processBB(Map &starting, CFGBlock *block) const;
        virtual DefinitionLevel mergeBlank(DefinitionLevel into) const {
            assert(into != DefinednessAnalysis::Undefined);
            return DefinednessAnalysis::PotentiallyDefined;
        }
};
class DefinednessVisitor : public ASTVisitor {
    private:
        typedef DefinednessBBAnalyzer::Map Map;
        Map &state;

        void _doSet(const std::string &s) {
            state[s] = DefinednessAnalysis::Defined;
        }

        void _doSet(AST* t) {
            switch (t->type) {
                case AST_TYPE::Attribute:
                    // doesn't affect definedness (yet?)
                    break;
                case AST_TYPE::Name:
                    _doSet(((AST_Name*)t)->id);
                    break;
                case AST_TYPE::Subscript:
                    break;
                case AST_TYPE::Tuple: {
                    AST_Tuple *tt = static_cast<AST_Tuple*>(t);
                    for (int i = 0; i < tt->elts.size(); i++) {
                        _doSet(tt->elts[i]);
                    }
                    break;
                }
                default:
                    ASSERT(0, "Unknown type for DefinednessVisitor: %d", t->type);
            }
        }
    public:
        DefinednessVisitor(Map &state) : state(state) {
        }

        virtual bool visit_branch(AST_Branch* node) { return true; }
        virtual bool visit_expr(AST_Expr* node) { return true; }
        virtual bool visit_global(AST_Global* node) { return true; }
        virtual bool visit_jump(AST_Jump* node) { return true; }
        virtual bool visit_pass(AST_Pass* node) { return true; }
        virtual bool visit_print(AST_Print* node) { return true; }
        virtual bool visit_return(AST_Return* node) { return true; }

        virtual bool visit_classdef(AST_ClassDef *node) {
            _doSet(node->name);
            return true;
        }

        virtual bool visit_functiondef(AST_FunctionDef *node) {
            _doSet(node->name);
            return true;
        }

        virtual bool visit_import(AST_Import *node) {
            for (int i = 0; i < node->names.size(); i++) {
                AST_alias *alias = node->names[i];
                std::string &name = alias->name;
                if (alias->asname.size())
                    name = alias->asname;

                _doSet(name);
            }
            return true;
        }

        virtual bool visit_assign(AST_Assign *node) {
            for (int i = 0; i < node->targets.size(); i++) {
                _doSet(node->targets[i]);
            }
            return true;
        }

        virtual bool visit_arguments(AST_arguments *node) {
            if (node->kwarg) _doSet(node->kwarg);
            if (node->vararg.size()) _doSet(node->vararg);
            for (int i = 0; i < node->args.size(); i++) {
                _doSet(node->args[i]);
            }
            return true;
        }
};
void DefinednessBBAnalyzer::processBB(Map &starting, CFGBlock *block) const {
    DefinednessVisitor visitor(starting);
    for (int i = 0; i < block->body.size(); i++) {
        block->body[i]->accept(&visitor);
    }
    if (block->idx == 0 && arguments) {
        arguments->accept(&visitor);
    }
}

DefinednessAnalysis::DefinednessAnalysis(AST_arguments *args, CFG* cfg, ScopeInfo *scope_info) : scope_info(scope_info) {
    results = computeFixedPoint(cfg, DefinednessBBAnalyzer(args), false);

    for (std::unordered_map<CFGBlock*, std::unordered_map<std::string, DefinitionLevel> >::iterator
            it = results.begin(), end = results.end(); it != end; ++it) {
        RequiredSet required;
        for (std::unordered_map<std::string, DefinitionLevel>::iterator it2 = it->second.begin(), end2 = it->second.end();
                it2 != end2; ++it2) {
            if (scope_info->refersToGlobal(it2->first))
                continue;

            //printf("%d %s %d\n", it->first->idx, it2->first.c_str(), it2->second);
            required.insert(it2->first);
        }
        defined.insert(make_pair(it->first, required));
    }
}

DefinednessAnalysis::DefinitionLevel DefinednessAnalysis::isDefinedAt(const std::string &name, CFGBlock *block) {
    std::unordered_map<std::string, DefinitionLevel> &map = results[block];
    if (map.count(name) == 0)
        return Undefined;
    return map[name];
}

const DefinednessAnalysis::RequiredSet& DefinednessAnalysis::getDefinedNamesAt(CFGBlock *block) {
    return defined[block];
}

PhiAnalysis::PhiAnalysis(AST_arguments* args, CFG* cfg, LivenessAnalysis *liveness, ScopeInfo *scope_info) :
        definedness(args, cfg, scope_info), liveness(liveness) {
    for (int i = 0; i < cfg->blocks.size(); i++) {
        CFGBlock *block = cfg->blocks[i];

        RequiredSet required;
        if (block->predecessors.size() < 2)
            continue;

        const RequiredSet& defined = definedness.getDefinedNamesAt(block);
        if (defined.size())
            assert(block->predecessors.size());
        for (RequiredSet::const_iterator it = defined.begin(), end = defined.end(); it != end; ++it) {
            if (liveness->isLiveAtEnd(*it, block->predecessors[0])) {
                required.insert(*it);
            }
        }

        required_phis.insert(make_pair(block, required));
    }
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredAfter(CFGBlock *block) {
    static RequiredSet empty;
    if (block->successors.size() == 0)
        return empty;
    return required_phis[block->successors[0]];
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllDefinedAt(CFGBlock *block) {
    return definedness.getDefinedNamesAt(block);
}

bool PhiAnalysis::isRequired(const std::string &name, CFGBlock* block) {
    return required_phis[block].count(name) != 0;
}

bool PhiAnalysis::isRequiredAfter(const std::string &name, CFGBlock* block) {
    // If there are multiple successors, then none of them are allowed
    // to require any phi nodes
    if (block->successors.size() != 1)
        return false;

    // Fall back to the other method:
    return isRequired(name, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAfter(const std::string &name, CFGBlock* block) {
    assert(block->successors.size() > 0);
    DefinednessAnalysis::DefinitionLevel dlevel = definedness.isDefinedAt(name, block->successors[0]);
    ASSERT(dlevel != DefinednessAnalysis::Undefined, "%s %d", name.c_str(), block->idx);

    return dlevel == DefinednessAnalysis::PotentiallyDefined;
}

LivenessAnalysis* computeLivenessInfo(CFG*) {
    return new LivenessAnalysis();
}

PhiAnalysis* computeRequiredPhis(AST_arguments* args, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo *scope_info) {
    return new PhiAnalysis(args, cfg, liveness, scope_info);
}

}
