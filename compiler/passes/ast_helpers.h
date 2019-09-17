//
//  ast_helpers.hpp
//  floyd
//
//  Created by Marcus Zetterquist on 2019-07-30.
//  Copyright © 2019 Marcus Zetterquist. All rights reserved.
//

#ifndef ast_helpers_hpp
#define ast_helpers_hpp

#include <vector>
#include <memory>

namespace floyd {

struct general_purpose_ast_t;
struct function_definition_t;
struct expression_t;
struct statement_t;
struct body_t;
struct typeid_t;
struct ast_type_t;
struct type_interner_t;
struct struct_definition_t;

bool check_types_resolved(const type_interner_t& interner, const expression_t& e);
bool check_types_resolved(const type_interner_t& interner, const std::vector<expression_t>& expressions);

bool check_types_resolved(const type_interner_t& interner, const function_definition_t& def);

bool check_types_resolved(const type_interner_t& interner, const body_t& body);

bool check_types_resolved(const type_interner_t& interner, const statement_t& s);
bool check_types_resolved(const type_interner_t& interner, const std::vector<std::shared_ptr<statement_t>>& s);

bool check_types_resolved(const type_interner_t& interner, const struct_definition_t& s);
bool check_types_resolved(const type_interner_t& interner, const ast_type_t& t);
bool check_types_resolved(const type_interner_t& interner, const typeid_t& t);

bool check_types_resolved(const type_interner_t& interner);
bool check_types_resolved(const general_purpose_ast_t& ast);

bool check_types_resolved__type_vector(const type_interner_t& interner, const std::vector<typeid_t>& elements);


}	//	floyd


#endif /* ast_helpers_hpp */
