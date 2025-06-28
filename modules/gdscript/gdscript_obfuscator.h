/**************************************************************************/
/*  gdscript_obfuscator.h                                                 */
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

#ifndef GDSCRIPT_OBFUSCATOR_H
#define GDSCRIPT_OBFUSCATOR_H

#include "core/io/config_file.h"
#include "core/os/mutex.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/typedefs.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "modules/gdscript/gdscript_parser.h"

class GDScriptObfuscator {
	Mutex mutex;
	GDScriptParser *gdscript_parser;
	HashMap<StringName, String> source_symbols;
	HashSet<StringName> locked_symbols;
	HashSet<StringName> property_cache;
	HashMap<String, String> uid_map;
	Vector<String> declarations;
	bool remove_prints = false;

	// builtin tests
	bool _is_builtin(const GDScriptParser::IdentifierNode *p_id);

	// value rewrites
	String _rewrite_annotation(GDScriptParser::AnnotationNode *p_annotation, uint64_t *p_seed);
	String _rewrite_type(GDScriptParser::TypeNode *p_type, uint64_t *p_seed);
	String _rewrite_param(GDScriptParser::ParameterNode *p_param, uint64_t *p_seed);
	String _rewrite_subscript(GDScriptParser::SubscriptNode *p_subscript, uint64_t *p_seed);
	String _rewrite_call(GDScriptParser::CallNode *p_call, uint64_t *p_seed);
	String _rewrite_literal(GDScriptParser::LiteralNode *p_literal);
	String _rewrite_string(const Variant &p_string);
	String _rewrite_array(GDScriptParser::ArrayNode *p_array, uint64_t *p_seed);
	String _rewrite_dictionary(GDScriptParser::DictionaryNode *p_dict, uint64_t *p_seed);
	String _rewrite_preload(GDScriptParser::PreloadNode *p_preload, uint64_t *p_seed);
	String _rewrite_identifier(GDScriptParser::IdentifierNode *p_id, uint64_t *p_seed);

	// expression rewrites
	String _rewrite_assign(GDScriptParser::AssignmentNode *p_op, uint64_t *p_seed);
	String _rewrite_binaryop(GDScriptParser::BinaryOpNode *p_op, uint64_t *p_seed);
	String _rewrite_unaryop(GDScriptParser::UnaryOpNode *p_op, uint64_t *p_seed);
	String _rewrite_ternary(GDScriptParser::TernaryOpNode *p_op, uint64_t *p_seed);
	String _rewrite_test(GDScriptParser::TypeTestNode *p_op, uint64_t *p_seed);
	String _rewrite_cast(GDScriptParser::CastNode *p_op, uint64_t *p_seed);
	String _rewrite_return(GDScriptParser::ReturnNode *p_op, uint64_t *p_seed);
	String _rewrite_getnode(GDScriptParser::GetNodeNode *p_op);
	String _rewrite_await(GDScriptParser::AwaitNode *p_op, uint64_t *p_seed);
	String _rewrite_pattern(GDScriptParser::PatternNode *p_pattern, uint64_t *p_seed);
	inline String _rewrite_dict_pattern(const GDScriptParser::PatternNode::Pair &p_pair, uint64_t *p_seed) { return _rewrite_node(p_pair.key, p_seed) + ":" + _rewrite_pattern(p_pair.value_pattern, p_seed); }

	// composite statement rewrites
	String _rewrite_lambda(GDScriptParser::LambdaNode *p_lambda, uint64_t *p_seed, String p_indent = "");
	String _rewrite_suite(GDScriptParser::SuiteNode *p_suite, uint64_t *p_seed, String p_indent = "");
	String _rewrite_if(GDScriptParser::IfNode *p_cond, uint64_t *p_seed, String p_indent = "");
	String _rewrite_while(GDScriptParser::WhileNode *p_cond, uint64_t *p_seed, String p_indent = "");
	String _rewrite_for(GDScriptParser::ForNode *p_cond, uint64_t *p_seed, String p_indent = "");
	String _rewrite_match(GDScriptParser::MatchNode *p_cond, uint64_t *p_seed, String p_indent = "");
	String _rewrite_case(GDScriptParser::MatchBranchNode *p_cond, uint64_t *p_seed, String p_indent = "");

	// class member rewrites
	String _rewrite_const(GDScriptParser::ConstantNode *p_constant, uint64_t *p_seed, String p_indent = "");
	String _rewrite_var(GDScriptParser::VariableNode *p_constant, uint64_t *p_seed, String p_indent = "");
	String _rewrite_enum(GDScriptParser::EnumNode *p_enum, uint64_t *p_seed, String p_indent = "");
	String _rewrite_signal(GDScriptParser::SignalNode *p_signal, uint64_t *p_seed, String p_indent = "");
	String _rewrite_func(GDScriptParser::FunctionNode *p_func, uint64_t *p_seed, String p_indent = "");
	String _rewrite_class(GDScriptParser::ClassNode *p_class, uint64_t *p_seed, bool p_is_inner = true, String p_indent = "");
	String _rewrite_node(GDScriptParser::Node *p_node, uint64_t *p_seed, String p_indent = "");
	String inline _group_rewrite(GDScriptParser::Node *p_node, uint64_t *p_seed) { return "(" + _rewrite_node(p_node, p_seed) + ")"; }

	// pre obfuscation operations
	void _pre_parse_class(GDScriptParser::ClassNode *p_class, uint64_t *p_seed);
	void _pre_parse_script(const String &p_path, uint64_t *p_seed);
	void _pre_parse_project(const String &p_path);
	void _pre_parse_directory(const String &p_path, uint64_t *p_seed);

public:
	/*
	 * Toggles the removing of 'print' statements on next obfuscations.
	 */
	inline void set_remove_prints(bool p_enabled) { remove_prints = p_enabled; }

	/*
	 * Resets all symbols stored from all obfuscations.
	 */
	void reset_symbols();

	/*
	 * Parse and pre obfuscate all class level symbols on scripts in the 'res://' of the project.
	 * This provides the obfuscated symbols, locked identifiers and UIDs paths for the script rewriting.
	 */
	void obfuscate_script_classes(uint64_t *p_seed);

	/*
	 * Obfuscates the given source code String. This source shall come from a GDScript or the operation will fail.
	 * `obfuscate_script_classes` is necessary for the correct usage of UID paths and locked symbols.
	 *
	 * When `remove_prints` is set to `true`, all 'print' statements will be removed and only
	 * 'push_warning', 'push_error' and 'push_info' will remain.
	 */
	String obfuscate_source_code(const String &p_source, const String &p_path, uint64_t *p_seed);

	/*
	 * Obfuscates the identifiers on the class cache array with symbols already obfuscated via `obfuscate_script_classes`.
	 */
	TypedArray<Dictionary> obfuscate_class_cache(const TypedArray<Dictionary> &class_cache);

	GDScriptObfuscator();
	virtual ~GDScriptObfuscator();
};

#endif // GDSCRIPT_OBFUSCATOR_H
