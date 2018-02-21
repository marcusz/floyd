//
//  pass2.hpp
//  FloydSpeak
//
//  Created by Marcus Zetterquist on 09/08/16.
//  Copyright © 2016 Marcus Zetterquist. All rights reserved.
//

#ifndef pass2_hpp
#define pass2_hpp

#include "ast.h"


struct json_t;

namespace floyd {
	struct ast_json_t;
	struct expression_t;

	/*
		Input is an array of statements from parser.
		A function has its own list of statements.
	*/
	const std::vector<std::shared_ptr<statement_t> > parser_statements_to_ast(const quark::trace_context_t& tracer, const ast_json_t& p);


	ast_json_t statement_to_json(const statement_t& e);
	std::vector<json_t> statements_shared_to_json(const std::vector<std::shared_ptr<statement_t>>& a);
	ast_json_t statements_to_json(const std::vector<std::shared_ptr<statement_t>>& e);



	ast_json_t expressions_to_json(const std::vector<expression_t> v);

	/*
		An expression is a json array where entries may be other json arrays.
		["+", ["+", 1, 2], ["k", 10]]
	*/
	ast_json_t expression_to_json(const expression_t& e);

	std::string expression_to_json_string(const expression_t& e);



	floyd::ast_t run_pass2(const quark::trace_context_t& tracer, const ast_json_t& parse_tree);
}
#endif /* pass2_hpp */
