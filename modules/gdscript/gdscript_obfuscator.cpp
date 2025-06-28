/**************************************************************************/
/*  gdscript_obfuscator.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_obfuscator.h"
#include "core/core_constants.h"
#include "core/core_globals.h"
#include "core/core_string_names.h"
#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/typedefs.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant.h"
#include "modules/gdscript/gdscript_cache.h"
#include "modules/gdscript/gdscript_parser.h"

static const int NAME_SIZE = 8;
static const char32_t *CHAR_TABLE = U"abcdefghijklmnopqrstuvxyzwABCDEFGHIJKLMNOPQRSTUVXYZW0123456789";
static const int CHAR_COUNT = strlen(CHAR_TABLE);
static const int INIT_CHAR_COUNT = CHAR_COUNT - 10; // remove numbers for 1st char on indetifiers
static HashSet<StringName> global_names;
static HashSet<StringName> print_statements;

/* STATIC UTILITY FUNCTIONS */

static inline bool _is_builtin_field(const ClassDB::ClassInfo &p_field, const StringName &p_name) {
	return p_field.method_map.has(p_name) || p_field.constant_map.has(p_name) ||
			p_field.enum_map.has(p_name) || p_field.property_map.has(p_name) ||
			p_field.signal_map.has(p_name);
}

static inline bool _is_variant_field(Variant::Type p_type, const StringName &p_name) {
	return Variant::has_member(p_type, p_name) || Variant::has_builtin_method(p_type, p_name) ||
			Variant::has_constant(p_type, p_name) || Variant::has_enum(p_type, p_name);
}

static int _find_annotation(const List<GDScriptParser::AnnotationNode *> &p_annotations, const String &p_beginning) {
	for (int i = 0; i < p_annotations.size(); ++i) {
		if (String(p_annotations.get(i)->name).begins_with(p_beginning)) {
			return i;
		}
	}
	return -1;
}

static inline bool _is_to_keep_name(List<GDScriptParser::AnnotationNode *> &p_annotations) {
	return _find_annotation(p_annotations, "@keep") >= 0;
}

static inline bool _is_exported(List<GDScriptParser::AnnotationNode *> &p_annotations) {
	return _find_annotation(p_annotations, "@export") >= 0 && _find_annotation(p_annotations, "@export_tool_button") < 0;
}

static bool _is_potential_raw(const String &p_string) {
	// it could be a raw string if a \ followed by any character appears
	bool could_be_raw = p_string.size() > 1 && p_string.find_char('\\') >= 0;

	// if any escaped character appears however, it has to be regular escaped (except null termination)
	if (could_be_raw) {
		for (int i = 0; i < p_string.size(); ++i) {
			if (p_string[i] > '\0' && p_string[i] <= '\r') {
				could_be_raw = false;
				break;
			}
		}
	}

	return could_be_raw;
}

static bool _can_oneline_suite(GDScriptParser::SuiteNode *p_suite) {
	bool can_oneline = p_suite && p_suite->statements.size() <= 1;

	if (can_oneline) {
		switch (p_suite->statements[0]->type) {
			case GDScriptParser::Node::IF:
			case GDScriptParser::Node::WHILE:
			case GDScriptParser::Node::FOR:
			case GDScriptParser::Node::MATCH:
			case GDScriptParser::Node::MATCH_BRANCH:
				can_oneline = false;
				break;

			default:
				break;
		}
	}

	return can_oneline;
}

/*****************************/

bool GDScriptObfuscator::_is_builtin(const GDScriptParser::IdentifierNode *p_id) {
	// if already obfuscated, then it is not a builtin
	if (source_symbols.has(p_id->name)) {
		return false;
	}

	// let's see if it is a field from a raw type then
	for (int i = Variant::Type::NIL; i < Variant::Type::VARIANT_MAX; ++i) {
		if (_is_variant_field(static_cast<Variant::Type>(i), p_id->name)) {
			return true;
		}
	}

	// ok, then test if it is a global name, builtin class property or builtin class name
	if (global_names.has(p_id->name) || ClassDB::class_exists(p_id->name) || property_cache.has(p_id->name)) {
		return true;
	}

	if (CoreConstants::is_global_constant(p_id->name) || CoreConstants::is_global_enum(p_id->name)) {
		return true;
	}

	// test if property exists in any builtin and register it if it does
	for (const auto &class_data : ClassDB::classes) {
		if (_is_builtin_field(class_data.value, p_id->name)) {
			property_cache.insert(p_id->name);
			return true;
		}
	}

	return false;
}

void GDScriptObfuscator::reset_symbols() {
	MutexLock<Mutex> mutex_lock(mutex);

	source_symbols.clear();
	locked_symbols.clear();

	if (gdscript_parser) {
		gdscript_parser->cleanup();
	}
}

String GDScriptObfuscator::_rewrite_string(const Variant &p_string) {
	String str = p_string.get_type() == Variant::STRING_NAME ? "&\"" : "\"";

	// some string magics first: is the string a resource path? If so, "obfuscate it" with the UID path instead
	if (uid_map.has(p_string)) {
		str += uid_map[p_string];
	} else if (p_string.get_type() == Variant::STRING && _is_potential_raw(p_string)) {
		// second, attempt to detect if it could/should be a raw string
		str = "r" + str + String(p_string);
	} else {
		// third, if none of the above, escape special chars so they don't break the string on rewriting
		str += String(p_string).c_escape();
	}

	return str + "\"";
}

String GDScriptObfuscator::_rewrite_literal(GDScriptParser::LiteralNode *p_literal) {
	return p_literal->value.is_string() ? _rewrite_string(p_literal->value) : p_literal->value.get_construct_string();
}

String GDScriptObfuscator::_rewrite_type(GDScriptParser::TypeNode *p_type, uint64_t *p_seed) {
	String obfuscated_type;

	// handle the type first
	for (GDScriptParser::IdentifierNode *type : p_type->type_chain) {
		if (type != *(p_type->type_chain.begin())) {
			obfuscated_type += ".";
		}

		obfuscated_type += _rewrite_identifier(type, p_seed);
	}

	// in case of typed arrays and dctionaries, add the inner types
	if (!p_type->container_types.is_empty()) {
		obfuscated_type += "[";

		for (GDScriptParser::TypeNode *inner_type : p_type->container_types) {
			if (inner_type != *(p_type->container_types.begin())) {
				obfuscated_type += ",";
			}

			obfuscated_type += _rewrite_type(inner_type, p_seed);
		}

		obfuscated_type += "]";
	}

	return obfuscated_type;
}

String GDScriptObfuscator::_rewrite_param(GDScriptParser::ParameterNode *p_param, uint64_t *p_seed) {
	String obfuscated_param = _rewrite_identifier(p_param->identifier, p_seed);

	if (p_param->infer_datatype) {
		obfuscated_param += ":=" + _rewrite_node(p_param->initializer, p_seed);
	} else {
		if (p_param->datatype_specifier) {
			obfuscated_param += ":" + _rewrite_type(p_param->datatype_specifier, p_seed);
		}

		if (p_param->initializer) {
			obfuscated_param += "=" + _rewrite_node(p_param->initializer, p_seed);
		}
	}

	return obfuscated_param;
}
String GDScriptObfuscator::_rewrite_call(GDScriptParser::CallNode *p_call, uint64_t *p_seed) {
	String obfuscated_call;

	if (p_call->is_super) {
		obfuscated_call += "super";
	}

	if (p_call->callee) {
		if (!obfuscated_call.is_empty()) {
			obfuscated_call += ".";
		}

		// optional case to remove print statement calls
		if (remove_prints && p_call->callee->type == GDScriptParser::Node::IDENTIFIER) {
			GDScriptParser::IdentifierNode *callee_name = static_cast<GDScriptParser::IdentifierNode *>(p_call->callee);
			if (print_statements.has(callee_name->name)) {
				return "pass"; // replace print statement with pass to prevent problems in conditional blocks
			}
		}

		obfuscated_call += _rewrite_node(p_call->callee, p_seed);
	}

	obfuscated_call += "(";

	for (GDScriptParser::ExpressionNode *argument : p_call->arguments) {
		if (argument != *(p_call->arguments.begin())) {
			obfuscated_call += ",";
		}

		obfuscated_call += _rewrite_node(argument, p_seed);
	}

	return obfuscated_call + ")";
}

String GDScriptObfuscator::_rewrite_subscript(GDScriptParser::SubscriptNode *p_subscript, uint64_t *p_seed) {
	String obfuscated_subscript = _rewrite_node(p_subscript->base, p_seed);

	if (p_subscript->is_attribute) {
		obfuscated_subscript += "." + _rewrite_identifier(p_subscript->attribute, p_seed);
	} else {
		obfuscated_subscript += "[" + _rewrite_node(p_subscript->index, p_seed) + "]";
	}

	return obfuscated_subscript;
}

String GDScriptObfuscator::_rewrite_identifier(GDScriptParser::IdentifierNode *p_identifier, uint64_t *p_seed) {
	// in case of builtin or locked names, return the normal identifier name
	if (locked_symbols.has(p_identifier->name) || _is_builtin(p_identifier)) {
		return p_identifier->name;
	}

	if (!source_symbols.has(p_identifier->name)) {
		String obfuscated_id;

		// generate a random string based on the provided seed
		char32_t c = CHAR_TABLE[Math::rand_from_seed(p_seed) % INIT_CHAR_COUNT];
		for (int i = 0; i < NAME_SIZE; i++) {
			obfuscated_id += c;
			c = CHAR_TABLE[Math::rand_from_seed(p_seed) % CHAR_COUNT];
			*p_seed += c; // add entropy to seed
		}

		// mask the name of the identifier
		source_symbols.insert(p_identifier->name, obfuscated_id);
	}

	return source_symbols[p_identifier->name];
}

String GDScriptObfuscator::_rewrite_binaryop(GDScriptParser::BinaryOpNode *p_op, uint64_t *p_seed) {
	String obfuscated_op = "(" + _rewrite_node(p_op->left_operand, p_seed);

	switch (p_op->operation) {
		case GDScriptParser::BinaryOpNode::OP_ADDITION:
			obfuscated_op += "+";
			break;

		case GDScriptParser::BinaryOpNode::OP_SUBTRACTION:
			obfuscated_op += "-";
			break;

		case GDScriptParser::BinaryOpNode::OP_MULTIPLICATION:
			obfuscated_op += "*";
			break;

		case GDScriptParser::BinaryOpNode::OP_DIVISION:
			obfuscated_op += "/";
			break;

		case GDScriptParser::BinaryOpNode::OP_MODULO:
			obfuscated_op += "%";
			break;

		case GDScriptParser::BinaryOpNode::OP_POWER:
			obfuscated_op += "**";
			break;

		case GDScriptParser::BinaryOpNode::OP_BIT_LEFT_SHIFT:
			obfuscated_op += "<<";
			break;

		case GDScriptParser::BinaryOpNode::OP_BIT_RIGHT_SHIFT:
			obfuscated_op += ">>";
			break;

		case GDScriptParser::BinaryOpNode::OP_BIT_AND:
			obfuscated_op += "&";
			break;

		case GDScriptParser::BinaryOpNode::OP_BIT_OR:
			obfuscated_op += "|";
			break;

		case GDScriptParser::BinaryOpNode::OP_BIT_XOR:
			obfuscated_op += "^";
			break;

		case GDScriptParser::BinaryOpNode::OP_LOGIC_AND:
			obfuscated_op += "&&";
			break;

		case GDScriptParser::BinaryOpNode::OP_LOGIC_OR:
			obfuscated_op += "||";
			break;

		case GDScriptParser::BinaryOpNode::OP_CONTENT_TEST:
			obfuscated_op += " in ";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_EQUAL:
			obfuscated_op += "==";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_GREATER_EQUAL:
			obfuscated_op += ">=";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_LESS_EQUAL:
			obfuscated_op += "<=";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_GREATER:
			obfuscated_op += ">";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_LESS:
			obfuscated_op += "<";
			break;

		case GDScriptParser::BinaryOpNode::OP_COMP_NOT_EQUAL:
			obfuscated_op += "!=";
			break;
	}

	return obfuscated_op + _rewrite_node(p_op->right_operand, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_unaryop(GDScriptParser::UnaryOpNode *p_op, uint64_t *p_seed) {
	String obfuscated_op = "(";

	switch (p_op->operation) {
		case GDScriptParser::UnaryOpNode::OP_LOGIC_NOT:
			obfuscated_op += "!";
			break;

		case GDScriptParser::UnaryOpNode::OP_NEGATIVE:
			obfuscated_op += "-";
			break;

		case GDScriptParser::UnaryOpNode::OP_POSITIVE:
			obfuscated_op += "+";
			break;

		case GDScriptParser::UnaryOpNode::OP_COMPLEMENT:
			obfuscated_op += "~";
			break;
	}

	return obfuscated_op + _rewrite_node(p_op->operand, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_ternary(GDScriptParser::TernaryOpNode *p_op, uint64_t *p_seed) {
	return _group_rewrite(p_op->true_expr, p_seed) + "if" + _group_rewrite(p_op->condition, p_seed) + "else" + _group_rewrite(p_op->false_expr, p_seed);
}

String GDScriptObfuscator::_rewrite_const(GDScriptParser::ConstantNode *p_constant, uint64_t *p_seed, String p_indent) {
	return p_indent + "const " + _rewrite_identifier(p_constant->identifier, p_seed) + ":=" + _rewrite_node(p_constant->initializer, p_seed);
}

String GDScriptObfuscator::_rewrite_preload(GDScriptParser::PreloadNode *p_preload, uint64_t *p_seed) {
	return "preload(" + _rewrite_node(p_preload->path, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_getnode(GDScriptParser::GetNodeNode *p_op) {
	return (p_op->use_dollar ? "$\"" + p_op->full_path : "%\"" + p_op->full_path.substr(1)) + "\"";
}

String GDScriptObfuscator::_rewrite_enum(GDScriptParser::EnumNode *p_enum, uint64_t *p_seed, String p_indent) {
	String obfuscated_enum;

	// named enums are obfuscated normally
	if (p_enum->identifier) {
		obfuscated_enum += p_indent + "enum " + _rewrite_identifier(p_enum->identifier, p_seed) + "{";
		bool first = true;

		for (GDScriptParser::EnumNode::Value &entry : p_enum->values) {
			if (!first) {
				obfuscated_enum += ",";
			}

			first = false;
			obfuscated_enum += _rewrite_identifier(entry.identifier, p_seed);
			if (entry.custom_value) {
				obfuscated_enum += "=" + _rewrite_node(entry.custom_value, p_seed);
			}
		}

		obfuscated_enum += "}";
	} else {
		// unnamed enums are converted into constants
		for (GDScriptParser::EnumNode::Value &entry : p_enum->values) {
			obfuscated_enum += p_indent + "const " + _rewrite_identifier(entry.identifier, p_seed) + "=";

			if (entry.custom_value) {
				obfuscated_enum += _rewrite_node(entry.custom_value, p_seed);
			} else {
				obfuscated_enum += String::num_int64(entry.value);
			}

			obfuscated_enum += "\n";
		}
	}

	return obfuscated_enum;
}

String GDScriptObfuscator::_rewrite_assign(GDScriptParser::AssignmentNode *p_op, uint64_t *p_seed) {
	String obfuscated_op = _rewrite_node(p_op->assignee, p_seed);

	if (p_op->use_conversion_assign) {
		obfuscated_op += ":=";
	} else {
		switch (p_op->operation) {
			case GDScriptParser::AssignmentNode::OP_NONE:
				obfuscated_op += "=";
				break;
			case GDScriptParser::AssignmentNode::OP_ADDITION:
				obfuscated_op += "+=";
				break;
			case GDScriptParser::AssignmentNode::OP_SUBTRACTION:
				obfuscated_op += "-=";
				break;
			case GDScriptParser::AssignmentNode::OP_MULTIPLICATION:
				obfuscated_op += "*=";
				break;
			case GDScriptParser::AssignmentNode::OP_DIVISION:
				obfuscated_op += "/=";
				break;
			case GDScriptParser::AssignmentNode::OP_MODULO:
				obfuscated_op += "%=";
				break;
			case GDScriptParser::AssignmentNode::OP_POWER:
				obfuscated_op += "**=";
				break;
			case GDScriptParser::AssignmentNode::OP_BIT_SHIFT_LEFT:
				obfuscated_op += "<<=";
				break;
			case GDScriptParser::AssignmentNode::OP_BIT_SHIFT_RIGHT:
				obfuscated_op += ">>=";
				break;
			case GDScriptParser::AssignmentNode::OP_BIT_AND:
				obfuscated_op += "&=";
				break;
			case GDScriptParser::AssignmentNode::OP_BIT_OR:
				obfuscated_op += "|=";
				break;
			case GDScriptParser::AssignmentNode::OP_BIT_XOR:
				obfuscated_op += "^=";
				break;
		}
	}

	return obfuscated_op + "(" + _rewrite_node(p_op->assigned_value, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_return(GDScriptParser::ReturnNode *p_op, uint64_t *p_seed) {
	return p_op->return_value ? "return(" + _rewrite_node(p_op->return_value, p_seed) + ")" : "return";
}

String GDScriptObfuscator::_rewrite_test(GDScriptParser::TypeTestNode *p_op, uint64_t *p_seed) {
	return "((" + _rewrite_node(p_op->operand, p_seed) + ")is " + _rewrite_type(p_op->test_type, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_cast(GDScriptParser::CastNode *p_op, uint64_t *p_seed) {
	return "((" + _rewrite_node(p_op->operand, p_seed) + ")as " + _rewrite_type(p_op->cast_type, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_await(GDScriptParser::AwaitNode *p_op, uint64_t *p_seed) {
	return "await(" + _rewrite_node(p_op->to_await, p_seed) + ")";
}

String GDScriptObfuscator::_rewrite_annotation(GDScriptParser::AnnotationNode *p_annotation, uint64_t *p_seed) {
	String annotation = "@" + p_annotation->name + "(";

	for (GDScriptParser::ExpressionNode *arg : p_annotation->arguments) {
		if (arg != *(p_annotation->arguments.begin())) {
			annotation += ",";
		}

		annotation += _rewrite_node(arg, p_seed);
	}

	annotation += ") ";

	return annotation;
}

String GDScriptObfuscator::_rewrite_node(GDScriptParser::Node *p_node, uint64_t *p_seed, String p_indent) {
	switch (p_node->type) {
		case GDScriptParser::Node::ARRAY:
			return _rewrite_array(static_cast<GDScriptParser::ArrayNode *>(p_node), p_seed);
		case GDScriptParser::Node::ASSERT:
			return "pass"; // replace assert declarations with no-op
		case GDScriptParser::Node::ASSIGNMENT:
			return _rewrite_assign(static_cast<GDScriptParser::AssignmentNode *>(p_node), p_seed);
		case GDScriptParser::Node::AWAIT:
			return _rewrite_await(static_cast<GDScriptParser::AwaitNode *>(p_node), p_seed);
		case GDScriptParser::Node::BINARY_OPERATOR:
			return _rewrite_binaryop(static_cast<GDScriptParser::BinaryOpNode *>(p_node), p_seed);
		case GDScriptParser::Node::BREAK:
			return "break";
		case GDScriptParser::Node::CALL:
			return _rewrite_call(static_cast<GDScriptParser::CallNode *>(p_node), p_seed);
		case GDScriptParser::Node::CAST:
			return _rewrite_cast(static_cast<GDScriptParser::CastNode *>(p_node), p_seed);
		case GDScriptParser::Node::CLASS:
			return _rewrite_class(static_cast<GDScriptParser::ClassNode *>(p_node), p_seed, true, p_indent);
		case GDScriptParser::Node::CONSTANT:
			return _rewrite_const(static_cast<GDScriptParser::ConstantNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::CONTINUE:
			return "continue";
		case GDScriptParser::Node::DICTIONARY:
			return _rewrite_dictionary(static_cast<GDScriptParser::DictionaryNode *>(p_node), p_seed);
		case GDScriptParser::Node::ENUM:
			return _rewrite_enum(static_cast<GDScriptParser::EnumNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::FOR:
			return _rewrite_for(static_cast<GDScriptParser::ForNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::FUNCTION:
			return _rewrite_func(static_cast<GDScriptParser::FunctionNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::GET_NODE:
			return _rewrite_getnode(static_cast<GDScriptParser::GetNodeNode *>(p_node));
		case GDScriptParser::Node::IDENTIFIER:
			return _rewrite_identifier(static_cast<GDScriptParser::IdentifierNode *>(p_node), p_seed);
		case GDScriptParser::Node::IF:
			return _rewrite_if(static_cast<GDScriptParser::IfNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::LAMBDA:
			return _rewrite_lambda(static_cast<GDScriptParser::LambdaNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::LITERAL:
			return _rewrite_literal(static_cast<GDScriptParser::LiteralNode *>(p_node));
		case GDScriptParser::Node::MATCH:
			return _rewrite_match(static_cast<GDScriptParser::MatchNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::MATCH_BRANCH:
			return _rewrite_case(static_cast<GDScriptParser::MatchBranchNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::PARAMETER:
			return _rewrite_param(static_cast<GDScriptParser::ParameterNode *>(p_node), p_seed);
		case GDScriptParser::Node::PASS:
			return "pass";
		case GDScriptParser::Node::PATTERN:
			return _rewrite_pattern(static_cast<GDScriptParser::PatternNode *>(p_node), p_seed);
		case GDScriptParser::Node::PRELOAD:
			return _rewrite_preload(static_cast<GDScriptParser::PreloadNode *>(p_node), p_seed);
		case GDScriptParser::Node::RETURN:
			return _rewrite_return(static_cast<GDScriptParser::ReturnNode *>(p_node), p_seed);
		case GDScriptParser::Node::SELF:
			return "self";
		case GDScriptParser::Node::SIGNAL:
			return _rewrite_signal(static_cast<GDScriptParser::SignalNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::SUBSCRIPT:
			return _rewrite_subscript(static_cast<GDScriptParser::SubscriptNode *>(p_node), p_seed);
		case GDScriptParser::Node::SUITE:
			return _rewrite_suite(static_cast<GDScriptParser::SuiteNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::TERNARY_OPERATOR:
			return _rewrite_ternary(static_cast<GDScriptParser::TernaryOpNode *>(p_node), p_seed);
		case GDScriptParser::Node::TYPE:
			return _rewrite_type(static_cast<GDScriptParser::TypeNode *>(p_node), p_seed);
		case GDScriptParser::Node::TYPE_TEST:
			return _rewrite_test(static_cast<GDScriptParser::TypeTestNode *>(p_node), p_seed);
		case GDScriptParser::Node::UNARY_OPERATOR:
			return _rewrite_unaryop(static_cast<GDScriptParser::UnaryOpNode *>(p_node), p_seed);
		case GDScriptParser::Node::VARIABLE:
			return _rewrite_var(static_cast<GDScriptParser::VariableNode *>(p_node), p_seed, p_indent);
		case GDScriptParser::Node::WHILE:
			return _rewrite_while(static_cast<GDScriptParser::WhileNode *>(p_node), p_seed, p_indent);
		default:
			break;
	}

	return "";
}

String GDScriptObfuscator::_rewrite_suite(GDScriptParser::SuiteNode *p_suite, uint64_t *p_seed, String p_indent) {
	String obfuscated_suite;

	for (GDScriptParser::Node *statement : p_suite->statements) {
		obfuscated_suite += p_indent + _rewrite_node(statement, p_seed, p_indent) + "\n";
	}

	return obfuscated_suite.strip_edges(false, true);
}

String GDScriptObfuscator::_rewrite_signal(GDScriptParser::SignalNode *p_signal, uint64_t *p_seed, String p_indent) {
	String obfuscated_sig = p_indent + "signal " + _rewrite_identifier(p_signal->identifier, p_seed);

	if (!p_signal->parameters.is_empty()) {
		obfuscated_sig += "(";

		for (GDScriptParser::ParameterNode *param : p_signal->parameters) {
			if (param != *(p_signal->parameters.begin())) {
				obfuscated_sig += ",";
			}

			obfuscated_sig += _rewrite_param(param, p_seed);
		}

		obfuscated_sig += ")";
	}

	return obfuscated_sig;
}

String GDScriptObfuscator::_rewrite_lambda(GDScriptParser::LambdaNode *p_lambda, uint64_t *p_seed, String p_indent) {
	return _rewrite_func(p_lambda->function, p_seed, p_indent + "\t\t");
}

String GDScriptObfuscator::_rewrite_array(GDScriptParser::ArrayNode *p_array, uint64_t *p_seed) {
	String obfuscated_array = "[";

	for (GDScriptParser::ExpressionNode *expr : p_array->elements) {
		if (expr != *(p_array->elements.begin())) {
			obfuscated_array += ",";
		}

		obfuscated_array += _rewrite_node(expr, p_seed);
	}

	return obfuscated_array + "]";
}

String GDScriptObfuscator::_rewrite_dictionary(GDScriptParser::DictionaryNode *p_dict, uint64_t *p_seed) {
	String obfuscated_dict = "{";
	bool first = true;

	for (GDScriptParser::DictionaryNode::Pair &entry : p_dict->elements) {
		if (!first) {
			obfuscated_dict += ",";
		}

		first = false;
		obfuscated_dict += _rewrite_node(entry.key, p_seed) + ":" + _rewrite_node(entry.value, p_seed);
	}

	return obfuscated_dict + "}";
}

String GDScriptObfuscator::_rewrite_func(GDScriptParser::FunctionNode *p_func, uint64_t *p_seed, String p_indent) {
	String rpc;
	int rpc_index = _find_annotation(p_func->annotations, "@rpc");
	if (rpc_index >= 0) {
		rpc = _rewrite_annotation(p_func->annotations.get(rpc_index), p_seed);
	}

	String obfuscated_func = rpc + (p_func->is_static && p_func->identifier ? "static func" : "func") +
			(p_func->identifier ? " " + _rewrite_identifier(p_func->identifier, p_seed) : "") + "(";

	// obfuscate each parameter
	if (!p_func->parameters.is_empty()) {

		for (GDScriptParser::ParameterNode *param : p_func->parameters) {
			if (param != *(p_func->parameters.begin())) {
				obfuscated_func += ",";
			}

			obfuscated_func += _rewrite_param(param, p_seed);
		}
	}

	obfuscated_func += ")";

	// handle the return type of the function
	if (p_func->return_type && !p_func->return_type->type_chain.is_empty()) {
		obfuscated_func += "->" + _rewrite_type(p_func->return_type, p_seed);
	}

	if (_can_oneline_suite(p_func->body)) {
		obfuscated_func += ":" + _rewrite_node(p_func->body->statements[0], p_seed, p_indent + "\t");
	} else {
		obfuscated_func += ":\n" + _rewrite_suite(p_func->body, p_seed, p_indent + "\t");
	}

	return obfuscated_func;
}

String GDScriptObfuscator::_rewrite_if(GDScriptParser::IfNode *p_cond, uint64_t *p_seed, String p_indent) {
	String obfuscated_if = "if(" + _rewrite_node(p_cond->condition, p_seed) + "):\n";

	if (p_cond->true_block) {
		obfuscated_if += _rewrite_suite(p_cond->true_block, p_seed, p_indent + "\t");
	}

	if (p_cond->false_block) {
		obfuscated_if += "\n" + p_indent + "else:\n" + _rewrite_suite(p_cond->false_block, p_seed, p_indent + "\t");
	}

	return obfuscated_if;
}

String GDScriptObfuscator::_rewrite_for(GDScriptParser::ForNode *p_cond, uint64_t *p_seed, String p_indent) {
	String obfuscated_for = "for " + _rewrite_identifier(p_cond->variable, p_seed);

	if (p_cond->datatype_specifier) {
		obfuscated_for += ":" + _rewrite_type(p_cond->datatype_specifier, p_seed);
	}

	obfuscated_for += " in " + _rewrite_node(p_cond->list, p_seed) + ":\n";

	if (p_cond->loop) {
		obfuscated_for += _rewrite_suite(p_cond->loop, p_seed, p_indent + "\t");
	}

	return obfuscated_for;
}

String GDScriptObfuscator::_rewrite_while(GDScriptParser::WhileNode *p_cond, uint64_t *p_seed, String p_indent) {
	return "while(" + _rewrite_node(p_cond->condition, p_seed) + "):\n" + _rewrite_suite(p_cond->loop, p_seed, p_indent + "\t");
}

String GDScriptObfuscator::_rewrite_match(GDScriptParser::MatchNode *p_cond, uint64_t *p_seed, String p_indent) {
	String obfuscated_match = "match(" + _rewrite_node(p_cond->test, p_seed) + "):\n";

	for (GDScriptParser::MatchBranchNode *match : p_cond->branches) {
		obfuscated_match += _rewrite_case(match, p_seed, p_indent + "\t") + "\n";
	}

	return obfuscated_match.strip_edges(false, true);
}

String GDScriptObfuscator::_rewrite_case(GDScriptParser::MatchBranchNode *p_cond, uint64_t *p_seed, String p_indent) {
	String obfuscated_case = p_indent;

	for (GDScriptParser::PatternNode *pattern : p_cond->patterns) {
		if (pattern != *(p_cond->patterns.begin())) {
			obfuscated_case += ",";
		}

		obfuscated_case += _rewrite_pattern(pattern, p_seed);
	}

	if (p_cond->guard_body) {
		obfuscated_case += " when(" + _rewrite_suite(p_cond->guard_body, p_seed) + ")";
	}

	return obfuscated_case + ":\n" + _rewrite_suite(p_cond->block, p_seed, p_indent + "\t");
}

String GDScriptObfuscator::_rewrite_pattern(GDScriptParser::PatternNode *p_pattern, uint64_t *p_seed) {
	String obfuscated_pattern;

	switch (p_pattern->pattern_type) {
		case GDScriptParser::PatternNode::PT_REST:
			obfuscated_pattern = "..";
			break;

		case GDScriptParser::PatternNode::PT_WILDCARD:
			obfuscated_pattern = "_";
			break;

		case GDScriptParser::PatternNode::PT_LITERAL:
			obfuscated_pattern = _rewrite_literal(p_pattern->literal);
			break;

		case GDScriptParser::PatternNode::PT_EXPRESSION:
			obfuscated_pattern = _rewrite_node(p_pattern->expression, p_seed);
			break;

		case GDScriptParser::PatternNode::PT_BIND:
			obfuscated_pattern = "var " + _rewrite_identifier(p_pattern->bind, p_seed);
			break;

		case GDScriptParser::PatternNode::PT_ARRAY:
			obfuscated_pattern = "[";
			for (GDScriptParser::PatternNode *pattern : p_pattern->array) {
				if (pattern != *(p_pattern->array.begin())) {
					obfuscated_pattern += ",";
				}

				obfuscated_pattern += _rewrite_pattern(pattern, p_seed);
			}
			obfuscated_pattern += "]";
			break;

		case GDScriptParser::PatternNode::PT_DICTIONARY:
			obfuscated_pattern = "{";
			for (GDScriptParser::PatternNode::Pair &entry : p_pattern->dictionary) {
				if (entry.key != p_pattern->dictionary.begin()->key) {
					obfuscated_pattern += ",";
				}

				obfuscated_pattern += _rewrite_dict_pattern(entry, p_seed);
			}
			obfuscated_pattern += "}";
			break;
	}

	return obfuscated_pattern;
}

String GDScriptObfuscator::_rewrite_var(GDScriptParser::VariableNode *p_var, uint64_t *p_seed, String p_indent) {
	String obfuscated_var = p_var->is_static ? "static var " : "var ";

	if (_find_annotation(p_var->annotations, "@onready") >= 0) {
		obfuscated_var = "@onready " + obfuscated_var;
	}

	// add export annotation to any export type, except @export_editor_button
	if (_is_exported(p_var->annotations)) {
		obfuscated_var = "@export " + obfuscated_var;
	}

	obfuscated_var += _rewrite_identifier(p_var->identifier, p_seed);

	if (p_var->infer_datatype) {
		obfuscated_var += ":";
	} else if (p_var->datatype_specifier) {
		obfuscated_var += ":" + _rewrite_type(p_var->datatype_specifier, p_seed);
	}

	if (p_var->initializer) {
		obfuscated_var += "=" + _rewrite_node(p_var->initializer, p_seed);
	}

	switch (p_var->property) {
		case GDScriptParser::VariableNode::PROP_INLINE:
			obfuscated_var += ":\n";
			if (p_var->getter) {
				obfuscated_var += p_indent + "\tget:\n" + _rewrite_suite(p_var->getter->body, p_seed, p_indent + "\t\t");
			}

			if (p_var->setter) {
				if (p_var->getter) {
					obfuscated_var += "\n";
				}

				obfuscated_var += p_indent + "\tset(" + _rewrite_identifier(p_var->setter_parameter, p_seed) + "):\n" +
						_rewrite_suite(p_var->setter->body, p_seed, p_indent + "\t\t");
			}
			break;

		case GDScriptParser::VariableNode::PROP_SETGET:
			obfuscated_var += ":";
			if (p_var->getter) {
				obfuscated_var += "get=" + _rewrite_identifier(p_var->getter_pointer, p_seed);
			}

			if (p_var->setter) {
				if (p_var->getter) {
					obfuscated_var += ",";
				}

				obfuscated_var += "set=" + _rewrite_identifier(p_var->setter_pointer, p_seed);
			}
			break;

		default:
			break;
	}

	return obfuscated_var;
}

String GDScriptObfuscator::_rewrite_class(GDScriptParser::ClassNode *p_class, uint64_t *p_seed, bool p_inner, String p_indent) {
	String obfuscated_class;
	Vector<String> obfuscated_members;

	// obfuscate the class name, if any
	if (p_class->identifier) {
		obfuscated_class = p_indent + String(p_inner ? "class " : "class_name ") + _rewrite_identifier(p_class->identifier, p_seed);
	}

	// obfuscate parent class name if not a builtin
	if (p_class->extends_used) {
		obfuscated_class += obfuscated_class.is_empty() ? "extends " : " extends ";

		for (GDScriptParser::IdentifierNode *parent : p_class->extends) {
			if (parent != *(p_class->extends.begin())) {
				obfuscated_class += ".";
			}

			obfuscated_class += _rewrite_identifier(parent, p_seed);
		}
	}

	// finish class declaration line
	if (!obfuscated_class.is_empty()) {
		obfuscated_class += p_inner ? ":\n" : "\n";
	}

	// recover static unload if present
	if (p_class->annotated_static_unload) {
		obfuscated_class = "@static_unload\n" + obfuscated_class;
	}

	// obfuscate all members of the class
	String indent = p_indent + (p_inner ? "\t" : "");
	GDScriptParser::Node *member_node;
	for (GDScriptParser::ClassNode::Member &member : p_class->members) {
		member_node = member.get_source_node();

		// identifier nodes at this level are unnamed enum entries
		if (member_node->type == GDScriptParser::Node::IDENTIFIER) {
			if (member_node->next->type == GDScriptParser::Node::ENUM) {
				obfuscated_members.append(indent + _rewrite_enum(static_cast<GDScriptParser::EnumNode *>(member_node->next), p_seed, indent));
			}
		} else if (member_node->type != GDScriptParser::Node::ANNOTATION) {
			// standalone annotations are not included
			obfuscated_members.append(indent + _rewrite_node(member_node, p_seed, indent));
		}
	}

	// randomly shuffle members with Fisher-Yates
	for (int i = obfuscated_members.size() - 1; i > 0; --i) {
		int j = Math::rand_from_seed(p_seed) % i;
		String tmp = obfuscated_members[i];
		obfuscated_members.set(i, obfuscated_members[j]);
		obfuscated_members.set(j, tmp);
		*p_seed += j; // add entropy to seed
	}

	// finally, recreate the sauce with obfuscated members
	for (const String &member : obfuscated_members) {
		obfuscated_class += member.strip_edges(false, true) + "\n";
	}

	return obfuscated_class;
}

void GDScriptObfuscator::_pre_parse_class(GDScriptParser::ClassNode *p_class, uint64_t *p_seed) {
	GDScriptParser::Node *member_node;

	// preemptively generate obfuscation symbols for the script's class name, if not kept
	if (p_class->identifier) {
		if (_is_to_keep_name(p_class->annotations)) {
			locked_symbols.insert(p_class->identifier->name);
		} else {
			_rewrite_identifier(p_class->identifier, p_seed);
		}
	}

	// preemptively generate obfuscation symbols for the class members, if not kept
	for (GDScriptParser::ClassNode::Member &member : p_class->members) {
		member_node = member.get_source_node();

		switch (member_node->type) {
			case GDScriptParser::Node::CLASS: {
				GDScriptParser::ClassNode *class_node = static_cast<GDScriptParser::ClassNode *>(member_node);
				if (_is_to_keep_name(member_node->annotations)) {
					locked_symbols.insert(class_node->identifier->name);
				} else {
					_rewrite_identifier(class_node->identifier, p_seed);
				}
				_pre_parse_class(class_node, p_seed);
			} break;

			case GDScriptParser::Node::CONSTANT:
				if (_is_to_keep_name(member_node->annotations)) {
					locked_symbols.insert(static_cast<GDScriptParser::ConstantNode *>(member_node)->identifier->name);
				} else {
					_rewrite_identifier(static_cast<GDScriptParser::ConstantNode *>(member_node)->identifier, p_seed);
				}
				break;

			case GDScriptParser::Node::FUNCTION:
				if (_is_to_keep_name(member_node->annotations)) {
					locked_symbols.insert(static_cast<GDScriptParser::FunctionNode *>(member_node)->identifier->name);
				} else {
					_rewrite_identifier(static_cast<GDScriptParser::FunctionNode *>(member_node)->identifier, p_seed);
				}
				break;

			case GDScriptParser::Node::VARIABLE:
				if (_is_to_keep_name(member_node->annotations) || _is_exported(member_node->annotations)) {
					locked_symbols.insert(static_cast<GDScriptParser::VariableNode *>(member_node)->identifier->name);
				} else {
					_rewrite_identifier(static_cast<GDScriptParser::VariableNode *>(member_node)->identifier, p_seed);
				}
				break;

			case GDScriptParser::Node::ENUM: {
				GDScriptParser::EnumNode *enum_node = static_cast<GDScriptParser::EnumNode *>(member_node);
				if (enum_node->identifier && _is_to_keep_name(enum_node->annotations)) {
					locked_symbols.insert(enum_node->identifier->name);
				} else {
					_rewrite_identifier(enum_node->identifier, p_seed);
				}
			} break;

			case GDScriptParser::Node::SIGNAL:
				if (_is_to_keep_name(member_node->annotations)) {
					locked_symbols.insert(static_cast<GDScriptParser::SignalNode *>(member_node)->identifier->name);
				} else {
					_rewrite_identifier(static_cast<GDScriptParser::SignalNode *>(member_node)->identifier, p_seed);
				}
				break;

			default:
				break;
		}
	}
}

void GDScriptObfuscator::_pre_parse_script(const String &p_path, uint64_t *p_seed) {
	String source_code = FileAccess::get_file_as_string(p_path);
	Error parse_result = gdscript_parser->parse(source_code, p_path, true);
	ERR_FAIL_COND_MSG(parse_result != OK, vformat("Failed to pre parse: %s", p_path));

	_pre_parse_class(gdscript_parser->get_tree(), p_seed);
}

void GDScriptObfuscator::_pre_parse_project(const String &p_path) {
	ConfigFile project;
	Error load_result = project.load(p_path);
	ERR_FAIL_COND_MSG(load_result != OK, vformat("Failed to pre parse: %s", p_path));

	List<String> autoloads;
	project.get_section_keys("autoload", &autoloads);
	for (const String &autoload : autoloads) {
		locked_symbols.insert(autoload);
	}
}

void GDScriptObfuscator::_pre_parse_directory(const String &p_path, uint64_t *p_seed) {
	ResourceUID::ID res_uid;
	String full_path;

	for (const String &file : DirAccess::get_files_at(p_path)) {
		full_path = p_path.path_join(file);

		res_uid = ResourceLoader::get_resource_uid(full_path);
		if (res_uid != ResourceUID::INVALID_ID) {
			uid_map.insert(full_path, ResourceUID::get_singleton()->id_to_text(res_uid));
		}

		if (file == "project.godot") {
			_pre_parse_project(full_path);
		} else if (file.get_extension() == "gd") {
			_pre_parse_script(full_path, p_seed);
		}
	}

	for (const String &dir : DirAccess::get_directories_at(p_path)) {
		_pre_parse_directory(p_path.path_join(dir), p_seed);
	}
}

void GDScriptObfuscator::obfuscate_script_classes(uint64_t *p_seed) {
	ERR_FAIL_COND_MSG(gdscript_parser == nullptr, "Obfuscation failed. The parser is null.");
	_pre_parse_directory("res://", p_seed);
}

String GDScriptObfuscator::obfuscate_source_code(const String &p_source, const String &p_path, uint64_t *p_seed) {
	MutexLock<Mutex> mutex_lock(mutex);
	ERR_FAIL_COND_V_MSG(gdscript_parser == nullptr, "", "Obfuscation failed. The parser is null.");

	Error parser_error = gdscript_parser->parse(p_source, p_path, true);
	ERR_FAIL_COND_V_MSG(parser_error != OK, "", "Obfuscation failed. Parser failed to parse " + p_path);

	return _rewrite_class(gdscript_parser->get_tree(), p_seed, false);
}

TypedArray<Dictionary> GDScriptObfuscator::obfuscate_class_cache(const TypedArray<Dictionary> &class_cache) {
	TypedArray<Dictionary> new_cache;
	Dictionary obfuscated_class;
	StringName class_name;

	for (const Variant &entry : class_cache) {
		obfuscated_class = entry.duplicate();

		obfuscated_class["is_tool"] = false;
		obfuscated_class["is_abstract"] = false;
		obfuscated_class["icon"] = "";

		class_name = obfuscated_class["base"];
		if (source_symbols.has(class_name)) {
			obfuscated_class["base"] = StringName(source_symbols[class_name]);
		}

		class_name = obfuscated_class["class"];
		if (source_symbols.has(class_name)) {
			obfuscated_class["class"] = StringName(source_symbols[class_name]);
		}

		new_cache.push_back(obfuscated_class);
	}

	return new_cache;
}

GDScriptObfuscator::GDScriptObfuscator() {
	gdscript_parser = memnew(GDScriptParser);

	if (unlikely(global_names.is_empty() || print_statements.is_empty())) {
		global_names.insert("_static_init");
		global_names.insert("AABB");
		global_names.insert("Array");
		global_names.insert("Basis");
		global_names.insert("Callable");
		global_names.insert("Color");
		global_names.insert("Dictionary");
		global_names.insert("INF");
		global_names.insert("NAN");
		global_names.insert("NodePath");
		global_names.insert("Object");
		global_names.insert("PackedByteArray");
		global_names.insert("PackedColorArray");
		global_names.insert("PackedFloat32Array");
		global_names.insert("PackedFloat64Array");
		global_names.insert("PackedInt32Array");
		global_names.insert("PackedInt64Array");
		global_names.insert("PackedStringArray");
		global_names.insert("PackedVector2Array");
		global_names.insert("PackedVector3Array");
		global_names.insert("PackedVector4Array");
		global_names.insert("PI");
		global_names.insert("Plane");
		global_names.insert("Projection");
		global_names.insert("Quaternion");
		global_names.insert("Rect2");
		global_names.insert("Rect2i");
		global_names.insert("RID");
		global_names.insert("Signal");
		global_names.insert("String");
		global_names.insert("StringName");
		global_names.insert("TAU");
		global_names.insert("Transform2D");
		global_names.insert("Transform3D");
		global_names.insert("Vector2");
		global_names.insert("Vector2i");
		global_names.insert("Vector3");
		global_names.insert("Vector3i");
		global_names.insert("Vector4");
		global_names.insert("Vector4i");
		global_names.insert("a");
		global_names.insert("a8");
		global_names.insert("abs");
		global_names.insert("absf");
		global_names.insert("absi");
		global_names.insert("acos");
		global_names.insert("acosh");
		global_names.insert("angle_difference");
		global_names.insert("asin");
		global_names.insert("asinh");
		global_names.insert("assert");
		global_names.insert("atan");
		global_names.insert("atan2");
		global_names.insert("atanh");
		global_names.insert("b");
		global_names.insert("b8");
		global_names.insert("basis");
		global_names.insert("bezier_derivative");
		global_names.insert("bezier_interpolate");
		global_names.insert("bind");
		global_names.insert("bool");
		global_names.insert("bytes_to_var");
		global_names.insert("bytes_to_var_with_objects");
		global_names.insert("call");
		global_names.insert("call_deferred");
		global_names.insert("ceil");
		global_names.insert("ceilf");
		global_names.insert("ceili");
		global_names.insert("changed");
		global_names.insert("char");
		global_names.insert("clamp");
		global_names.insert("clampf");
		global_names.insert("clampi");
		global_names.insert("convert");
		global_names.insert("cos");
		global_names.insert("cosh");
		global_names.insert("cubic_interpolate");
		global_names.insert("cubic_interpolate_angle");
		global_names.insert("cubic_interpolate_angle_in_time");
		global_names.insert("cubic_interpolate_in_time");
		global_names.insert("d");
		global_names.insert("db_to_linear");
		global_names.insert("deg_to_rad");
		global_names.insert("dict_to_inst");
		global_names.insert("ease");
		global_names.insert("end");
		global_names.insert("error_string");
		global_names.insert("exp");
		global_names.insert("float");
		global_names.insert("floor");
		global_names.insert("floorf");
		global_names.insert("floori");
		global_names.insert("fmod");
		global_names.insert("fposmod");
		global_names.insert("free");
		global_names.insert("g");
		global_names.insert("g8");
		global_names.insert("get_stack");
		global_names.insert("get_rid");
		global_names.insert("h");
		global_names.insert("hash");
		global_names.insert("instance_from_id");
		global_names.insert("inst_to_dict");
		global_names.insert("int");
		global_names.insert("inverse_lerp");
		global_names.insert("is_equal_approx");
		global_names.insert("is_finite");
		global_names.insert("is_inf");
		global_names.insert("is_instance_id_valid");
		global_names.insert("is_instance_of");
		global_names.insert("is_instance_valid");
		global_names.insert("is_nan");
		global_names.insert("is_same");
		global_names.insert("is_zero_approx");
		global_names.insert("len");
		global_names.insert("lerp");
		global_names.insert("lerp_angle");
		global_names.insert("lerpf");
		global_names.insert("linear_to_db");
		global_names.insert("load");
		global_names.insert("log");
		global_names.insert("max");
		global_names.insert("maxf");
		global_names.insert("maxi");
		global_names.insert("min");
		global_names.insert("minf");
		global_names.insert("mini");
		global_names.insert("move_toward");
		global_names.insert("nearest_po2");
		global_names.insert("normal");
		global_names.insert("notification");
		global_names.insert("origin");
		global_names.insert("pingpong");
		global_names.insert("position");
		global_names.insert("posmod");
		global_names.insert("pow");
		global_names.insert("preload");
		global_names.insert("print");
		print_statements.insert("print");
		global_names.insert("print_debug");
		print_statements.insert("print_debug");
		global_names.insert("print_rich");
		print_statements.insert("print_rich");
		global_names.insert("print_stack");
		print_statements.insert("print_stack");
		global_names.insert("print_verbose");
		print_statements.insert("print_verbose");
		global_names.insert("printerr");
		print_statements.insert("printerr");
		global_names.insert("printraw");
		print_statements.insert("printraw");
		global_names.insert("prints");
		print_statements.insert("prints");
		global_names.insert("printt");
		print_statements.insert("printt");
		global_names.insert("property_list_changed");
		global_names.insert("push_error");
		global_names.insert("push_warning");
		global_names.insert("r");
		global_names.insert("r8");
		global_names.insert("rad_to_deg");
		global_names.insert("rand_from_seed");
		global_names.insert("randf");
		global_names.insert("randf_range");
		global_names.insert("randfn");
		global_names.insert("randi");
		global_names.insert("randi_range");
		global_names.insert("randomize");
		global_names.insert("range");
		global_names.insert("remap");
		global_names.insert("rid_allocate_id");
		global_names.insert("rid_from_int64");
		global_names.insert("rotate_toward");
		global_names.insert("round");
		global_names.insert("roundf");
		global_names.insert("roundi");
		global_names.insert("s");
		global_names.insert("seed");
		global_names.insert("sign");
		global_names.insert("signf");
		global_names.insert("signi");
		global_names.insert("sin");
		global_names.insert("sinh");
		global_names.insert("size");
		global_names.insert("smoothstep");
		global_names.insert("snapped");
		global_names.insert("snappedf");
		global_names.insert("snappedi");
		global_names.insert("sqrt");
		global_names.insert("step_decimals");
		global_names.insert("str");
		global_names.insert("str_to_var");
		global_names.insert("super");
		global_names.insert("tan");
		global_names.insert("tanh");
		global_names.insert("type_convert");
		global_names.insert("type_exists");
		global_names.insert("type_string");
		global_names.insert("typeof");
		global_names.insert("v");
		global_names.insert("var_to_bytes");
		global_names.insert("var_to_bytes_with_objects");
		global_names.insert("var_to_str");
		global_names.insert("void");
		global_names.insert("w");
		global_names.insert("weakref");
		global_names.insert("wrap");
		global_names.insert("wrapf");
		global_names.insert("wrapi");
		global_names.insert("x");
		global_names.insert("y");
		global_names.insert("z");
	}
}

GDScriptObfuscator::~GDScriptObfuscator() {
	if (gdscript_parser) {
		memdelete(gdscript_parser);
		gdscript_parser = nullptr;
	}
}
