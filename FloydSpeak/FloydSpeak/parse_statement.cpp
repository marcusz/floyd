//
//  parser_statement.cpp
//  FloydSpeak
//
//  Created by Marcus Zetterquist on 26/07/16.
//  Copyright © 2016 Marcus Zetterquist. All rights reserved.
//

#include "parse_statement.h"

#include "parse_expression.h"
#include "parser_primitives.h"
#include "floyd_parser.h"
#include "json_support.h"
#include "json_parser.h"

namespace floyd_parser {
	using std::string;
	using std::vector;
	using std::pair;
	using std::make_shared;
	using std::shared_ptr;





	pair<json_t, seq_t> parse_block(const seq_t& s){
		const auto pos = skip_whitespace(s);
		QUARK_ASSERT(pos.first1() == "{");

		const auto b_str = get_balanced(pos);
		const auto body_str = seq_t(trim_ends(b_str.first));
		const auto body_statements2 = read_statements2(body_str);
		return { json_t::make_array({ "block", body_statements2.first }), b_str.second };
	}

QUARK_UNIT_TEST("", "parse_block()", "Block with two binds", ""){
	ut_compare_jsons(
		parse_block(seq_t(" { int x = 1; int y = 2; } ")).first,
		parse_json(seq_t(
			R"(
				[
					"block",
					[
						["bind","<int>","x",["k",1,"<int>"]],
						["bind","<int>","y",["k",2,"<int>"]]
					]
				]
			)"
		)).first
	);
}







	pair<json_t, seq_t> parse_return_statement(const seq_t& s){
		const auto token_pos = if_first(skip_whitespace(s), "return");
		QUARK_ASSERT(token_pos.first);
		const auto expression_pos = read_until(skip_whitespace(token_pos.second), ";");
		const auto expression1 = parse_expression_all(seq_t(expression_pos.first));
		const auto statement = json_t::make_array({ json_t("return"), expression1 });
		//	Skip trailing ";".
		const auto pos = skip_whitespace(expression_pos.second.rest1());
		return { statement, pos };
	}

	QUARK_UNIT_TESTQ("parse_return_statement()", ""){
		const auto result = parse_return_statement(seq_t("return 0;"));
		QUARK_TEST_VERIFY(json_to_compact_string(result.first) == R"(["return", ["k", 0, "<int>"]])");
		QUARK_TEST_VERIFY(result.second.get_s() == "");
	}

#if false
	QUARK_UNIT_TESTQ("parse_return_statement()", ""){
		const auto t = make_shared<expression_t>(expression_t::make_constant(123));
		
		QUARK_TEST_VERIFY((
			parse_return_statement("return \t123\t;\t\nxyz}") == pair<return_statement_t, string>(return_statement_t{t}, "xyz}")
		));
	}
#endif



	pair<json_t, seq_t> parse_assignment_statement(const seq_t& s){
		const auto token_pos = read_until(s, whitespace_chars);
		const auto type = token_pos.first;

		const auto variable_pos = read_until(skip_whitespace(token_pos.second), whitespace_chars + "=");
		const auto equal_rest = read_required_char(skip_whitespace(variable_pos.second), '=');
		const auto expression_pos = read_until(skip_whitespace(equal_rest), ";");

		const auto expression = parse_expression_all(seq_t(expression_pos.first));

		const auto statement = json_t::make_array({ "bind", "<" + type + ">", variable_pos.first, expression });

		//	Skip trailing ";".
		return { statement, expression_pos.second.rest1() };
	}

#if false
	QUARK_UNIT_TESTQ("parse_assignment_statement", "bool true"){
		const auto a = parse_assignment_statement("bool bb = true; \n");
		QUARK_TEST_VERIFY(a.first._bind_statement->_identifier == "bb");
		QUARK_TEST_VERIFY(*a.first._bind_statement->_expression->_constant == value_t(true));
		QUARK_TEST_VERIFY(a.second == " \n");
	}

	QUARK_UNIT_TESTQ("parse_assignment_statement", "bool false"){
		const auto a = parse_assignment_statement("bool bb = false; \n");
		QUARK_TEST_VERIFY(a.first._bind_statement->_identifier == "bb");
		QUARK_TEST_VERIFY(*a.first._bind_statement->_expression->_constant == value_t(false));
		QUARK_TEST_VERIFY(a.second == " \n");
	}

	QUARK_UNIT_TESTQ("parse_assignment_statement", "int"){
		const auto a = parse_assignment_statement("int a = 10; \n");
		QUARK_TEST_VERIFY(a.first._bind_statement->_identifier == "a");
		QUARK_TEST_VERIFY(*a.first._bind_statement->_expression->_constant == value_t(10));
		QUARK_TEST_VERIFY(a.second == " \n");
	}

	QUARK_UNIT_TESTQ("parse_assignment_statement", "float"){
		const auto a = parse_assignment_statement("float b = 0.3; \n");
		QUARK_TEST_VERIFY(a.first._bind_statement->_identifier == "b");
		QUARK_TEST_VERIFY(*a.first._bind_statement->_expression->_constant == value_t(0.3f));
		QUARK_TEST_VERIFY(a.second == " \n");
	}

	QUARK_UNIT_TESTQ("parse_assignment_statement", "function call"){
		const auto a = parse_assignment_statement("float test = log(\"hello\");\n");
		QUARK_TEST_VERIFY(a.first._bind_statement->_identifier == "test");
		QUARK_TEST_VERIFY(a.first._bind_statement->_expression->_call->_function.to_string() == "log");
		QUARK_TEST_VERIFY(a.first._bind_statement->_expression->_call->_inputs.size() == 1);
		QUARK_TEST_VERIFY(*a.first._bind_statement->_expression->_call->_inputs[0]._constant ==value_t("hello"));
		QUARK_TEST_VERIFY(a.second == "\n");
	}
#endif


//??? Idea: Have explicit whitespaces - fail to parse.


/*
		Parse: if (EXPRESSION) { THEN_STATEMENTS }

		if(a){
			a
		}
*/
std::pair<json_t, seq_t> parse_if(const seq_t& pos){
	std::pair<bool, seq_t> a = if_first(pos, "if");
	QUARK_ASSERT(a.first);

	const auto pos2 = skip_whitespace(a.second);
	read_required(pos2, "(");
	const auto condition_paranthesis = get_balanced(pos2);
	const auto condition = trim_ends(condition_paranthesis.first);

	const auto pos3 = skip_whitespace(condition_paranthesis.second);
	read_required(pos3, "{");
	const auto then_statements_paranthesis = get_balanced(pos3);
	const auto then_statements = trim_ends(then_statements_paranthesis.first);

	auto return_pos = then_statements_paranthesis.second;

	const auto condition2 = parse_expression_all(seq_t(condition));
	const auto then_statements2 = read_statements2(seq_t(then_statements)).first;

	return { json_t::make_array({ "if", condition2, then_statements2 }), return_pos };
}

/*
	Parse optional else { STATEMENTS } or chain of else-if
	Ex 1: "some other statements"
	Ex 2: "else { STATEMENTS }"
	Ex 3: "else if (EXPRESSION) { STATEMENTS }"
	Ex 4: "else if (EXPRESSION) { STATEMENTS } else { STATEMENTS }"
	Ex 5: "else if (EXPRESSION) { STATEMENTS } else if (EXPRESSION) { STATEMENTS } else { STATEMENTS }"
*/


std::pair<json_t, seq_t> parse_if_statement(const seq_t& pos){
	const auto if_statement2 = parse_if(pos);
	std::pair<bool, seq_t> else_start = if_first(skip_whitespace(if_statement2.second), "else");
	if(else_start.first){
		const auto pos2 = skip_whitespace(else_start.second);
		std::pair<bool, seq_t> elseif_pos = if_first(pos2, "if");
		if(elseif_pos.first){
			const auto elseif_statement2 = parse_if_statement(pos2);
			return { json_t::make_array(
				{ "if", if_statement2.first.get_array_n(1), if_statement2.first.get_array_n(2), json_t::make_array({elseif_statement2.first}) }),
				elseif_statement2.second
			};
		}
		else{
			read_required(pos2, "{");
			const auto else_statements_paranthesis = get_balanced(pos2);
			const auto else_statements = trim_ends(else_statements_paranthesis.first);
			const auto else_statements2 = read_statements2(seq_t(else_statements));

			return { json_t::make_array(
				{ "if", if_statement2.first.get_array_n(1), if_statement2.first.get_array_n(2), else_statements2.first }),
				else_statements_paranthesis.second
			};
		}
	}
	else{
		return if_statement2;
	}
}

QUARK_UNIT_TEST("", "parse_if_statement()", "if(){}", ""){
	ut_compare_jsons(
		parse_if_statement(seq_t("if (1 > 2) { return 3; }")).first,
		parse_json(seq_t(
			R"(
				[
					"if",
					[">",["k",1,"<int>"],["k",2,"<int>"]],
					[
						["return", ["k", 3, "<int>"]]
					]
				]
			)"
		)).first
	);
}

QUARK_UNIT_TEST("", "parse_if_statement()", "if(){}else{}", ""){
	ut_compare_jsons(
		parse_if_statement(seq_t("if (1 > 2) { return 3; } else { return 4; }")).first,
		parse_json(seq_t(
			R"(
				[
					"if",
					[">",["k",1,"<int>"],["k",2,"<int>"]],
					[
						["return", ["k", 3, "<int>"]]
					],
					[
						["return", ["k", 4, "<int>"]]
					]
				]
			)"
		)).first
	);
}

QUARK_UNIT_TEST("", "parse_if_statement()", "if(){}else{}", ""){
	ut_compare_jsons(
		parse_if_statement(seq_t("if (1 > 2) { return 3; } else { return 4; }")).first,
		parse_json(seq_t(
			R"(
				[
					"if",
					[">",["k",1,"<int>"],["k",2,"<int>"]],
					[
						["return", ["k", 3, "<int>"]]
					],
					[
						["return", ["k", 4, "<int>"]]
					]
				]
			)"
		)).first
	);
}

QUARK_UNIT_TEST("", "parse_if_statement()", "if(){} else if(){} else {}", ""){
	ut_compare_jsons(
		parse_if_statement(
			seq_t("if (1 == 1) { return 1; } else if(2 == 2) { return 2; } else if(3 == 3) { return 3; } else { return 4; }")
		).first,
		parse_json(seq_t(
			R"(
				[
					"if", ["==",["k",1,"<int>"],["k",1,"<int>"]],
					[
						["return", ["k", 1, "<int>"]]
					],
					[
						[ "if", ["==",["k",2,"<int>"],["k",2,"<int>"]],
							[
								["return", ["k", 2, "<int>"]]
							],
							[
								[ "if", ["==",["k",3,"<int>"],["k",3,"<int>"]],
									[
										["return", ["k", 3, "<int>"]]
									],
									[
										["return", ["k", 4, "<int>"]]
									]
								]
							]
						]
					]
				]
			)"
		)).first
	);
}

/*
	for ( INIT_STATEMENT ; CONDITION_EXPRESSION ; POST_STATEMENT ) { BODY_STATEMENTS }

	for ( int x = 0 ; x < 10 ; x++ ){
		print(x)
	}

	OUTPUT
		INIT_STATEMENT, CONDITION_EXPRESSION and POST_STATEMENT can also be null.
		[ "for", INIT_STATEMENT, CONDITION_EXPRESSION, POST_STATEMENT, [BODY_STATEMENT1, 2, 3] ]
		[ "for", null, CONDITION_EXPRESSION, null, [] ]
		[ "for", null, null, null, [] ]
*/
std::pair<json_t, seq_t> parse_for_statement(const seq_t& pos){
	std::pair<bool, seq_t> a = if_first(pos, "for");
	QUARK_ASSERT(a.first);

	const auto pos2 = skip_whitespace(a.second);

	if(pos2.first1_char() != '('){
		throw std::runtime_error("syntax error");
	}

	const auto header = get_balanced(pos2);
	const auto pos3 = skip_whitespace(header.second);
	const auto body = get_balanced(pos3);

	const auto init_statement_str = read_until(seq_t(trim_ends(header.first)), ";");
	const auto condition_expression_str = read_until(init_statement_str.second.rest1(), ";");
	const auto post_expression_str = condition_expression_str.second.rest1();

	const auto body_statements_str = trim_ends(body.first);


	const auto init_statement2 = read_statement2(seq_t(init_statement_str.first));
	const auto condition_expression2 = parse_expression_all(seq_t(condition_expression_str.first));
	const auto post_expression2 = parse_expression_all(seq_t(post_expression_str));

	const auto body_statements2 = read_statements2(seq_t(body_statements_str));

	const auto r = json_t::make_array({ "for", init_statement2.first, condition_expression2, post_expression2, body_statements2.first });

	return { r, body.second };
}

QUARK_UNIT_TEST("", "parse_for_statement()", "for(){}", ""){
	ut_compare_jsons(
		parse_for_statement(seq_t("for ( int x = 0 ; x < 10 ; x + 100 ) { int y = 11; }")).first,
		parse_json(seq_t(
			R"(
				[
					"for",
					["bind","<int>","x",["k",0,"<int>"]],
					["<",["@","x"],["k",10,"<int>"]],
					["+",["@","x"],["k",100,"<int>"]],
					[
						["bind","<int>","y",["k",11,"<int>"]]
					]
				]
			)"
		)).first
	);
}





}	//	floyd_parser
