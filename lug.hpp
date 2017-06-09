// lug - Embedded DSL for PE grammar parsers in C++
// Copyright (c) 2017 Jesse W. Towner

#ifndef LUG_HPP
#define LUG_HPP

#include <any>
#include <algorithm>
#include <functional>
#include <deque>
#include <iostream>
#include <locale>
#include <memory>
#include <numeric>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lug
{

class lug_error : public std::runtime_error { using std::runtime_error::runtime_error; };
class grammar_error : public lug_error { using lug_error::lug_error; };
class parser_error : public lug_error { using lug_error::lug_error; };
class program_error : public lug_error { using lug_error::lug_error; };

namespace utf8
{

constexpr bool is_lead(char c) noexcept
{
	return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
}

template <class InputIt>
constexpr std::size_t count_runes(InputIt first, InputIt last)
{
	return std::accumulate(first, last, std::size_t{0}, [&](std::size_t l, char c) { return l + is_lead(c); });
}

template <class InputIt>
constexpr InputIt next_rune(InputIt first, InputIt last)
{
	return first != last ? std::find_if(std::next(first), last, is_lead) : last;
}

template <class InputIt>
constexpr std::size_t size_of_first_rune(InputIt first, InputIt last)
{
	return static_cast<std::size_t>(std::distance(first, next_rune(first, last)));
}

template <class Input>
inline std::size_t size_of_first_rune(Input input, std::size_t pos, std::size_t len)
{
	auto first = std::next(std::cbegin(input), static_cast<std::ptrdiff_t>(pos));
	auto last = std::next(first, static_cast<std::ptrdiff_t>(len));
	return size_of_first_rune(first, last);
}

} // namespace utf8

template <class Error>
struct reentrancy_sentinel
{
	bool& value;
	reentrancy_sentinel(bool& x, const char* msg) : value{x} { if (value) throw Error{msg}; value = true; }
	~reentrancy_sentinel() { value = false; }
};

struct syntax_position { std::size_t column, line; };
struct syntax_range { std::size_t index, length; };
struct syntax_view { std::string_view match; syntax_position start, end; };

class parser;
typedef std::function<void(parser&)> parser_error_handler;
typedef std::function<bool(parser&)> parser_predicate;

class semantic_environment;
typedef std::function<void(semantic_environment&)> semantic_action;
typedef std::function<void(semantic_environment&, const syntax_view&)> syntax_action;

class rule;
class grammar;

enum class immediate : unsigned short { zero = 0 };

enum class opcode : unsigned char
{
	match,          match_any,      match_class,    match_range,
	choice,         commit,         jump,           call,
	ret,            fail,           accept,         newline,
	predicate,      action,         begin_capture,  end_capture
};

enum class operands : unsigned char { none = 0, off = 1, str = 2 };

constexpr operands operator&(operands x, operands y) noexcept
{
	return static_cast<operands>(static_cast<unsigned char>(x) & static_cast<unsigned char>(y));
}

constexpr operands operator|(operands x, operands y) noexcept
{
	return static_cast<operands>(static_cast<unsigned char>(x) | static_cast<unsigned char>(y));
}

union instruction
{
	static constexpr std::size_t maxstrlen = 256;

	struct prefix { opcode op; operands aux; unsigned short val; } pf;
	int off;
	char str[4];

	instruction(opcode op, operands aux, immediate imm) : pf{op, aux, static_cast<unsigned short>(imm)} {}
	instruction(std::ptrdiff_t o) : off{static_cast<int>(o)} { if (off != o) throw program_error{"offset exceeds allowable range"}; }
	instruction(std::string_view s) { std::fill(std::copy_n(s.begin(), (std::min)(s.size(), std::size_t{4}), &str[0]), &str[4], char{0}); }

	static opcode decode(const instruction* code, std::ptrdiff_t& pc, std::size_t& imm, std::ptrdiff_t& off, std::string_view& str) {
		const prefix pf = code[pc++].pf;
		imm = pf.val;
		if ((pf.aux & operands::off) == operands::off)
			off = code[pc++].off;
		else
			off = 0;
		if ((pf.aux & operands::str) == operands::str) {
			str = std::string_view{code[pc].str, (imm & 0xFF) + 1};
			pc += ((imm & 0xFF) + 4) >> 2;
			imm = (imm >> 8) + 1;
		} else {
			str = std::string_view{};
		}
		return pf.op;
	}

	static std::ptrdiff_t length(prefix p) noexcept {
		std::ptrdiff_t l = 1;
		if ((p.aux & operands::off) == operands::off)
			l++;
		if ((p.aux & operands::str) == operands::str)
			l += static_cast<std::ptrdiff_t>(((p.val & 0xFF) >> 2) + 1);
		return l;
	}
};

static_assert(sizeof(instruction) == sizeof(int), "expected instruction to be same size as int");
static_assert(sizeof(int) <= sizeof(std::ptrdiff_t), "expected int to be no larger than ptrdiff_t");

struct program
{
	std::vector<instruction> instructions;
	std::vector<parser_error_handler> error_handlers;
	std::vector<parser_predicate> predicates;
	std::vector<semantic_action> semantic_actions;
	std::vector<syntax_action> syntax_actions;

	void concatenate(const program& src) {
		instructions.reserve(instructions.size() + src.instructions.size());
		for (auto i = src.instructions.begin(), j = i, e = src.instructions.end(); i != e; i = j) {
			instruction instr = *i;
			std::size_t immoffset;
			switch (instr.pf.op) {
				case opcode::predicate: immoffset = predicates.size(); break;
				case opcode::action: immoffset = semantic_actions.size(); break;
				case opcode::end_capture: immoffset = syntax_actions.size(); break;
				default: immoffset = 0; break;
			}
			if (immoffset != 0) {
				std::size_t imm = instr.pf.val + immoffset;
				if (immoffset <= imm || imm > (std::numeric_limits<unsigned short>::max)())
					throw program_error("immediate value exceeds 16-bit limit");
				instr.pf.val = static_cast<unsigned short>(imm);
			}
			j = std::next(i, instruction::length(instr.pf));
			instructions.push_back(instr);
			instructions.insert(instructions.end(), i, j);
		}
		error_handlers.insert(error_handlers.end(), src.error_handlers.begin(), src.error_handlers.end());
		predicates.insert(predicates.end(), src.predicates.begin(), src.predicates.end());
		semantic_actions.insert(semantic_actions.end(), src.semantic_actions.begin(), src.semantic_actions.end());
		syntax_actions.insert(syntax_actions.end(), src.syntax_actions.begin(), src.syntax_actions.end());
	}

	void swap(program& p) {
		instructions.swap(p.instructions);
		error_handlers.swap(p.error_handlers);
		predicates.swap(p.predicates);
		semantic_actions.swap(p.semantic_actions);
		syntax_actions.swap(p.syntax_actions);
	}
};

namespace expr
{

// NOTE: remove after std::is_invocable becomes available in both libc++ and MSVC++ 2017
template <typename AlwaysVoid, typename, typename...> struct is_invocable_impl : std::false_type {};
template <typename F, typename...Args> struct is_invocable_impl<decltype(void(::std::invoke(::std::declval<F>(), ::std::declval<Args>()...))), F, Args...> : std::true_type {};
template <typename AlwaysVoid, typename, typename...> struct invoke_result_impl {};
template <typename F, typename...Args> struct invoke_result_impl<decltype(void(::std::invoke(::std::declval<F>(), ::std::declval<Args>()...))), F, Args...> { using type = decltype(::std::invoke(::std::declval<F>(), ::std::declval<Args>()...)); };
template <class F, class... ArgTypes> struct is_invocable : is_invocable_impl<void, F, ArgTypes...> {};
template <class F, class... ArgTypes> constexpr bool is_invocable_v = is_invocable<F, ArgTypes...>::value;
template <class F, class... ArgTypes> struct invoke_result : invoke_result_impl<void, F, ArgTypes...> {};
template <class F, class... ArgTypes> using invoke_result_t = typename invoke_result<F, ArgTypes...>::type;

class evaluator;
class rule_evaluator;
template <class E> constexpr bool is_callable_impl_v = std::is_same_v<grammar, E> || std::is_same_v<rule, E> || std::is_same_v<program, E>;
template <class E> constexpr bool is_callable_v = is_callable_impl_v<std::remove_reference_t<E>>;
template <class E> constexpr bool is_string_expression_v = std::is_convertible_v<E, std::string>;
template <class E> constexpr bool is_proper_expression_v = is_invocable_v<E, evaluator&>;
template <class E> constexpr bool is_expression_v = is_proper_expression_v<E> || is_callable_v<E> || is_string_expression_v<E> || std::is_same_v<char, E>;

} // namespace expr

class rule
{
	friend class expr::evaluator;
	friend class expr::rule_evaluator;
	friend grammar start(const rule&);
public:
	rule() = default;
	template <class E, class = std::enable_if_t<expr::is_expression_v<E>>> rule(const E& e);
	rule(const rule& r);
	rule(rule&& r) = default;
	rule& operator=(const rule& r) { rule{r}.swap(*this); return *this; }
	rule& operator=(rule&& r) = default;
	void swap(rule& r) { program_.swap(r.program_); references_.swap(r.references_); dependencies_.swap(r.dependencies_); }
private:
	lug::program program_;
	std::vector<std::pair<const lug::program*, std::ptrdiff_t>> references_;
	std::unordered_set<const rule*> dependencies_;
};

class grammar
{
	friend grammar start(const rule&);
public:
	grammar() = default;
	void swap(grammar& g) { program_.swap(g.program_); }
	const lug::program& program() const noexcept { return program_; };
private:
	grammar(lug::program p) : program_{std::move(p)} {}
	lug::program program_;
};

class semantic_environment
{
public:
	void accept(std::string_view c) {
		capture_ = c;
		while (!actions_.empty()) {
			semantic_action a{std::move(actions_.front())};
			actions_.pop_front();
			a(*this);
		}
	}

	void clear() {
		actions_.clear();
		attributes_.clear();
		capture_ = std::string_view{};
	}

	std::size_t action_count() const noexcept { return actions_.size(); }
	void push_action(semantic_action a) { actions_.push_back(std::move(a)); }
	void pop_action() { actions_.pop_back(); }
	void pop_actions_after(std::size_t n) { if (n < actions_.size()) actions_.resize(n); }
	void skip_action() { actions_.pop_front(); }

	template <class T> void push_attribute(T&& x) { attributes_.emplace_back(std::in_place_type<T>, ::std::forward<T>(x)); }
	template <class T, class... Args> void push_attribute(Args&&... args) { attributes_.emplace_back(std::in_place_type<T>, ::std::forward<Args>(args)...); }
	template <class T> T pop_attribute() { T r{::std::any_cast<T>(attributes_.back())}; attributes_.pop_back(); return r; }

	const std::string_view& capture() const { return capture_; }

private:
	std::deque<semantic_action> actions_;
	std::vector<std::any> attributes_;
	std::string_view capture_;
};

namespace expr
{

class evaluator
{
	virtual immediate do_add_error_handler(parser_error_handler h) { return immediate::zero; }
	virtual immediate do_add_predicate(parser_predicate) { return immediate::zero; }
	virtual immediate do_add_semantic_action(semantic_action) { return immediate::zero; }
	virtual immediate do_add_syntax_action(syntax_action) { return immediate::zero; }
	virtual void do_add_dependency(const rule*) {}
	virtual void do_add_reference(const program*, std::ptrdiff_t) {}
	virtual void do_append(instruction) = 0;
	virtual std::ptrdiff_t do_length() const noexcept = 0;

public:
	evaluator() = default;
	virtual ~evaluator() {}

	evaluator& append(instruction instr) {
		do_append(instr);
		return *this;
	}

	template <class InputIt>
	evaluator& append(InputIt first, InputIt last) {
		for ( ; first != last; ++first)
			do_append(*first);
		return *this;
	}

	evaluator& call(const program& p) { do_add_reference(&p, length() + 1); return encode(opcode::call, -1); }
	evaluator& call(const rule& r) { do_add_dependency(&r); return call(r.program_); }
	evaluator& call(const grammar& g) { return call(g.program()); }

	evaluator& encode(opcode op, immediate imm = immediate::zero) { return append(instruction{op, operands::none, imm}); }
	evaluator& encode(opcode op, parser_predicate p) { return append(instruction{op, operands::none, do_add_predicate(std::move(p))}); }
	evaluator& encode(opcode op, semantic_action a) { return append(instruction{op, operands::none, do_add_semantic_action(std::move(a))}); }
	evaluator& encode(opcode op, syntax_action a) { return append(instruction{op, operands::none, do_add_syntax_action(std::move(a))}); }
	evaluator& encode(opcode op, std::ptrdiff_t off, immediate imm = immediate::zero) { return append(instruction{op, operands::off, imm}).append(instruction{off}); }

	template <class E> auto evaluate(const E& e) -> std::enable_if_t<is_expression_v<E>, evaluator&>;
	std::ptrdiff_t length() const noexcept { return do_length(); }
};

class program_length_evaluator : public evaluator
{
	std::ptrdiff_t length_ = 0;
	std::ptrdiff_t do_length() const noexcept override { return length_; }

	void do_append(instruction instr) override {
		if (length_ >= (std::numeric_limits<std::ptrdiff_t>::max)())
			throw grammar_error{"program length exceeds limits"};
		++length_;
	}
};

class program_evaluator : public evaluator
{
	program& program_;
	immediate do_add_error_handler(parser_error_handler h) override { return add_item(program_.error_handlers, std::move(h)); }
	immediate do_add_predicate(parser_predicate p) override { return add_item(program_.predicates, std::move(p)); }
	immediate do_add_semantic_action(semantic_action a) override { return add_item(program_.semantic_actions, std::move(a)); }
	immediate do_add_syntax_action(syntax_action a) override { return add_item(program_.syntax_actions, std::move(a)); }
	std::ptrdiff_t do_length() const noexcept override { return static_cast<std::ptrdiff_t>(program_.instructions.size()); }

	void do_append(instruction instr) override {
		if (program_.instructions.size() >= (std::numeric_limits<std::ptrdiff_t>::max)())
			throw grammar_error{"program length exceeds limits"};
		program_.instructions.push_back(instr);
	}

	template <class Item>
	immediate add_item(std::vector<Item> items, Item&& item) {
		if (items.size() >= (std::numeric_limits<unsigned short>::max)())
			throw grammar_error{"number of objects exceeds internal limit"};
		items.push_back(std::forward<Item>(item));
		return static_cast<immediate>(items.size() - 1);
	}

public:
	explicit program_evaluator(program& p) : program_{p} {}
};

class rule_evaluator : public program_evaluator
{
	rule& rule_;
	void do_add_dependency(const rule* r) override { rule_.dependencies_.insert(r); }
	void do_add_reference(const program* p, std::ptrdiff_t off) override { rule_.references_.push_back(std::make_pair(p, off)); }
public:
	explicit rule_evaluator(rule& r) : program_evaluator{r.program_}, rule_{r} {}
};

template <class E> std::ptrdiff_t program_length(const E& e) { return program_length_evaluator{}.evaluate(e).length(); }

struct any_expression
{
	void operator()(evaluator& v) const { v.encode(opcode::match_any); }
};

struct char_expression
{
	char c;
	void operator()(evaluator& v) const { v.append(instruction{opcode::match, operands::str, immediate::zero}).append(std::string_view{&c, 1}); }
};

struct empty_expression
{
	void operator()(evaluator& v) const { v.encode(opcode::match); }
};

template <class Target>
struct call_expression
{
	const Target& target;
	void operator()(evaluator& v) const { v.call(target); }
};

class string_expression
{
public:
	string_expression(const std::string& s) { compile(s); }
	void operator()(evaluator& v) const { v.append(instructions_.begin(), instructions_.end()); }

private:
	void compile(const std::string& s) {
		// TODO: replace this all with a pre-compiled program once parser engine is working
		if (s.empty()) {
			instructions_.emplace_back(opcode::match, operands::none, immediate::zero);
			return;
		}
		std::string::size_type i = 0, j = 0, k = 0;
		for (;;) {
			k = s.find_first_of(".[", j);
			compile_sequence_match(std::string_view{s}.substr(i, k < std::string::npos ? k - i : k));
			if (k == std::string::npos)
				break;
			if (s[k] == '.') {
				instructions_.emplace_back(opcode::match_any, operands::none, immediate::zero);
			} else {
				i = (j = k)++;
				j += j < s.size() && s[j] == '^';
				j += j < s.size() && s[j] == ']';
				k = s.find_first_of(']', j);
				if (k == std::string::npos) throw grammar_error{"expected closing bracket ']'"};
				// bracket from i+1 k-1
			}
			i = j = k + 1;
		}
	}

	void compile_sequence_match(std::string_view sequence) {
		while (sequence.size() > instruction::maxstrlen) {
			std::string_view subsequence = sequence.substr(0, instruction::maxstrlen);
			while (!subsequence.empty() && !utf8::is_lead(subsequence.back()))
				subsequence.remove_suffix(1);
			subsequence.remove_suffix(!subsequence.empty());
			compile_subsequence_match(subsequence);
			sequence.remove_prefix(subsequence.size());
		}
		compile_subsequence_match(sequence);
	}

	void compile_subsequence_match(std::string_view subsequence) {
		if (subsequence.size() > instruction::maxstrlen)
			throw grammar_error{"subsequence exceeds allowed number of UTF8 octets"};
		if (!subsequence.empty()) {
			std::size_t runes = utf8::count_runes(subsequence.cbegin(), subsequence.cend());
			instructions_.emplace_back(opcode::match, operands::str, static_cast<immediate>(((runes - 1) << 8) | (subsequence.size() - 1)));
			do {
				instructions_.emplace_back(subsequence);
				subsequence.remove_prefix((std::min)(size_t{4}, subsequence.size()));
			} while (!subsequence.empty());
		}
	}

	std::vector<instruction> instructions_;
};

template <class T>
inline auto make_expression(const T& t) {
	static_assert(is_expression_v<T>, "T must be an expression type");
	if constexpr (is_callable_v<T>)
		return call_expression<T>{t};
	else if constexpr (std::is_same_v<char, T>)
		return char_expression{t};
	else if constexpr (is_string_expression_v<T>)
		return string_expression{t};
	else
		return t;
}

template <class T> struct make_expression_result { using type = decltype(make_expression(::std::declval<T>())); };
template <class T> using make_expression_result_t = typename make_expression_result<T>::type;

template <class E>
inline auto evaluator::evaluate(const E& e) -> std::enable_if_t<is_expression_v<E>, evaluator&> {
	make_expression(e)(*this);
	return *this;
}

namespace operators
{

using semantics = lug::semantic_environment&;
using syntax = const lug::syntax_view&;

template <class E, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator!(E&& e) {
	return [x = make_expression(::std::forward<E>(e))](evaluator& ev) {
		ev.encode(opcode::choice, 3 + program_length(x)).evaluate(x).encode(opcode::commit, 1).encode(opcode::fail);
	};
}

template <class E, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator*(E&& e) {
	return [x = make_expression(::std::forward<E>(e))](evaluator& ev) {
		const std::ptrdiff_t xlen = program_length(x);
		ev.encode(opcode::choice, 2 + xlen).evaluate(x).encode(opcode::commit, -(xlen + 4));
	};
}

template <class E1, class E2, class = std::enable_if_t<is_expression_v<E1> && is_expression_v<E2>>>
inline auto operator|(E1&& e1, E2&& e2) {
	return [x1 = make_expression(::std::forward<E1>(e1)), x2 = make_expression(::std::forward<E2>(e2))](evaluator& ev) {
		ev.encode(opcode::choice, program_length(x1)).evaluate(x1).encode(opcode::commit, program_length(x2)).evaluate(x2);
	};
}

template <class E1, class E2, class = std::enable_if_t<is_expression_v<E1> && is_expression_v<E2>>>
inline auto operator>(E1&& e1, E2&& e2) {
	return [x1 = make_expression(::std::forward<E1>(e1)), x2 = make_expression(::std::forward<E2>(e2))](evaluator& ev) {
		ev.evaluate(x1).evaluate(x2);
	};
}

template <class E, class A, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator<(E&& e, A a) {
	return [x = make_expression(::std::forward<E>(e)), a = ::std::move(a)](evaluator& ev) {
		if constexpr (is_invocable_v<A, semantics, syntax>) {
			ev.encode(opcode::begin_capture).evaluate(x).encode(opcode::end_capture, syntax_action{a});
		} else if constexpr (is_invocable_v<A, semantics>) {
			ev.evaluate(x).encode(opcode::action, semantic_action{a});
		} else if constexpr (is_invocable_v<A>) {
			ev.evaluate(x).encode(opcode::action, [a](semantics s) {
				if constexpr (std::is_same_v<void, invoke_result_t<A>>)
					a();
				else
					s.push_attribute(a());
			});
		}
	};
}

template <class E1, class E2, class = std::enable_if_t<is_expression_v<E1> && is_expression_v<E2>>>
inline auto operator>=(E1&& e1, E2&& e2) {
	return ::std::forward<E1>(e1) < [](semantics s, syntax x) { s.push_attribute(x.match); } > ::std::forward<E2>(e2);
}

template <class E, class A, class = std::enable_if_t<is_expression_v<E> && is_invocable_v<A, std::string_view>>>
inline auto operator<=(E&& e, A a) {
	return ::std::forward<E>(e) < [a = std::move(a)](semantics s) {
		if constexpr (std::is_same_v<void, invoke_result_t<A, std::string_view>>)
			a(s.pop_attribute<std::string_view>());
		else
			s.push_attribute(a(s.pop_attribute<std::string_view>()));
	};
}

template <class T, class E, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator%(T& x, E&& e) {
	return ::std::forward<E>(e) < [&x](semantics s) { x = s.pop_attribute<T>(); };
}

template <class E, class = std::enable_if_t<is_expression_v<E>>> inline auto operator+(E&& e) { return ::std::forward<E>(e) > *(::std::forward<E>(e)); }
template <class E, class = std::enable_if_t<is_expression_v<E>>> inline auto operator&(E&& e) { return !(!(::std::forward<E>(e))); }
template <class E, class = std::enable_if_t<is_expression_v<E>>> inline auto operator~(E&& e) { return ::std::forward<E>(e) | empty_expression{}; }

} // namespace operators

} // namespace expr

template <class E, class> inline rule::rule(const E& e) { expr::rule_evaluator{*this}.evaluate(e); }
inline rule::rule(const rule& r) { expr::rule_evaluator{*this}.call(r); /* jmp */ }

inline grammar start(const rule& start_rule) {
	std::unordered_map<const program*, std::ptrdiff_t> addresses;
	std::vector<std::pair<const program*, std::ptrdiff_t>> references;
	std::stack<const rule*> unprocessed;
	std::unordered_set<const rule*> visited;
	program p;
	unprocessed.push(&start_rule);
	do {
		const rule* r = unprocessed.top();
		unprocessed.pop();
		if (visited.count(r) != 0)
			continue;
		visited.insert(r);
		auto addr = addresses.emplace(&r->program_, static_cast<std::ptrdiff_t>(p.instructions.size()));
		if (addr.second) {
			p.concatenate(r->program_);
			p.instructions.emplace_back(opcode::ret, operands::none, immediate::zero);
		}
		for (auto ref : r->references_) {
			references.emplace_back(ref.first, ref.second + addr.first->second);
			if (addresses.emplace(ref.first, static_cast<std::ptrdiff_t>(p.instructions.size())).second) {
				p.concatenate(*ref.first);
				p.instructions.emplace_back(opcode::ret, operands::none, immediate::zero);
			}
		}
		for (auto dep : r->dependencies_)
			unprocessed.push(dep);
	} while (!unprocessed.empty());
	for (auto ref : references) {
		instruction& instr = p.instructions[ref.second];
		std::ptrdiff_t reladdr = instr.off + addresses[ref.first] - ref.second;
		if (reladdr < std::numeric_limits<int>::lowest() || std::numeric_limits<int>::max() < reladdr)
			throw grammar_error("program offset exceeds addressable range");
		instr.off = static_cast<int>(reladdr);
	}
	return grammar(std::move(p));
}

class parser
{
public:
	struct options {};

	parser(const grammar& g, const options& o)
			: grammar_{g}
			, options_(o)
			, parsing_{false}
			, reading_{false} {
		reset();
	}

	bool parse() {
		reentrancy_sentinel<parser_error> guard{parsing_, "lug::parser::parse is non-reenterant"};

		const program& prog = grammar_.program();
		const instruction* const instr = prog.instructions.data();
		std::size_t imm, ir, nr, cr, lr, ac, fc = 0;
		std::ptrdiff_t off, pc;
		std::string_view str;

		load_registers(ir, nr, cr, lr, ac, pc);

		while (nr > 0 || read_more(ir, nr)) {
			switch (instruction::decode(instr, pc, imm, off, str)) {
				case opcode::match:
					if (str.size() > 0) {
						if (!available(str.size(), ir, nr))
							goto failure;
						if (input_.compare(ir, str.size(), str) != 0)
							goto failure;
						advance(str.size(), imm, ir, nr, cr);
					}
					break;
				case opcode::match_any:
					if (!available(1, ir, nr))
						goto failure;
					advance(utf8::size_of_first_rune(input_, ir, nr), 1, ir, nr, cr);
					break;
				case opcode::match_class:
					// TODO
					break;
				case opcode::match_range:
					// TODO
					break;
				case opcode::choice:
					stack_frames_.push_back(stack_frame_type::backtrack);
					//backtrack_stack_.push_back(std::make_tuple(program_state{ac,pc+off}, {{ir+(imm&255),nr},{cr+(imm>>8),lr},ir+nr == input_.size()}));
					break;
				case opcode::commit:
					switch (stack_frames_.back()) {
						case stack_frame_type::backtrack: backtrack_stack_.pop_back(); break;
						default: /*error*/ break;
					}
					stack_frames_.pop_back();
					pc += off;
					break;
				case opcode::call:
					stack_frames_.push_back(stack_frame_type::call);
					call_stack_.push_back(program_state{ac,pc});
					pc += off;
					break;
				case opcode::jump:
					pc += off;
					break;
				case opcode::ret:
					switch (stack_frames_.back()) {
						case stack_frame_type::call: pc = call_stack_.back().program_counter; call_stack_.pop_back(); break;
						//case stack_frame_type::call_left: pc = call_left_stack_.back().; call_left_stack_.pop_back(); break;
						default: /*error*/ break;
					}
					stack_frames_.pop_back();
					break;
				case opcode::fail:
					fc = imm;
					goto failure;
				case opcode::accept:
					// TODO
					break;
				case opcode::newline:
					cr = 1;
					lr += 1;
					break;
				case opcode::predicate:
					save_registers(ir, nr, cr, lr, ac, pc);
					if (!prog.predicates[imm](*this))
						goto failure;
					load_registers(ir, nr, cr, lr, ac, pc);
					break;
				case opcode::action:
					semantics_.push_action(prog.semantic_actions[imm]);
					ac = semantics_.action_count();
					break;
				case opcode::begin_capture:
					// push input state
					break;
				case opcode::end_capture:
					// pop input state
					//semantics_.push_action([a = prog.syntax_actions[imm], x](const semantic_environment& s) { a(s, x); });
					ac = semantics_.action_count();
					break;
				default:
					// invalid opcode
					break;
			}
			continue;
		failure:
			save_registers(ir, nr, cr, lr, ac, pc);
			for (++fc; fc > 0; --fc) {
				if (stack_frames_.empty())
					return false;
				stack_frame_type type = stack_frames_.back();
				stack_frames_.pop_back();
				switch (type) {
					case stack_frame_type::backtrack: {
						const auto& frame = backtrack_stack_.back();
						program_state_ = std::get<0>(frame);
						input_state_ = std::get<1>(frame);
						backtrack_stack_.pop_back();
					} break;
					case stack_frame_type::call: {
						program_state_ = call_stack_.back();
						call_stack_.pop_back();
						++fc;
					} break;
					case stack_frame_type::call_left: {
						const auto& frame = call_left_stack_.back();
						program_state_ = std::get<0>(frame);
						input_state_ = std::get<3>(frame);
						call_left_stack_.pop_back();
					} break;
					default: break;
				}
			}
			load_registers(ir, nr, cr, lr, ac, pc);
		}

		save_registers(ir, nr, cr, lr, ac, pc);
		semantics_.accept(input_);
		return true;
	}

	template <class InputFunc>
	parser& push_source(InputFunc&& func) {
		if (!reading_)
			throw parser_error("new input source cannot be specified while reading from input sources");
		sources_.emplace_back(::std::forward<InputFunc>(func));
		return *this;
	}

	template <class InputIt>
	parser& enqueue(InputIt first, InputIt last) {
		input_.insert(input_.end(), first, last);
		if (input_state_.eof)
			input_state_.range.length = input_.size() - input_state_.range.index;
		return *this;
	}

	void save_registers(std::size_t ir, std::size_t nr, std::size_t cr, std::size_t lr, std::size_t ac, std::ptrdiff_t pc) {
		input_state_ = {{ir, nr}, {cr, lr}, ir + nr == input_.size()};
		program_state_ = {ac, pc};
	}

	void load_registers(std::size_t& ir, std::size_t& nr, std::size_t& cr, std::size_t& lr, std::size_t& ac, std::ptrdiff_t& pc) {
		ir = input_state_.range.index;
		nr = input_state_.eof ? input_.size() - ir : input_state_.range.length;
		cr = input_state_.position.column;
		lr = input_state_.position.line;
		ac = program_state_.action_counter;
		pc = program_state_.program_counter;
		semantics_.pop_actions_after(ac);
	}

	/*
	void push_syntax_match(state& s, syntax_action a)
	{
		semantics_.push_action([a = std::move(a), m = syntax_match{std::get<0>(s), begin_, std::get<2>(s), position_}](semantic_environment& e) { a(e, m); });
	}*/

	std::string_view input_view() const noexcept { return std::string_view{input_.data() + input_state_.range.index, input_state_.range.length}; }
	const syntax_position& input_position() const noexcept { return input_state_.position; }
	void input_position(const syntax_position& position) noexcept { input_state_.position = position; }
	void input_position(std::size_t column, std::size_t line) noexcept { input_position(syntax_position{column, line}); }

private:
	enum class stack_frame_type : unsigned char { backtrack, call, call_left };
	struct syntax_state { syntax_range range; syntax_position position; bool eof; };
	struct program_state { std::size_t action_counter; std::ptrdiff_t program_counter; };

	void advance(std::size_t n, std::size_t c, std::size_t& ir, std::size_t& nr, std::size_t& cr) {
		ir += n;
		nr -= n;
		cr += c;
	}

	bool available(std::size_t n, std::size_t ir, std::size_t& nr) {
		do {
			if (n <= nr)
				return true;
			if (ir + nr < input_.size())
				return false;
		} while (read_more(ir, nr));
		return false;
	}

	bool read_more(std::size_t ir, std::size_t& nr) {
		reentrancy_sentinel<parser_error> guard{reading_, "lug::parser::read_more is non-reenterant"};
		input_state_.range = {ir, nr};
		input_state_.eof = ir + nr == input_.size();
		std::string text;
		while (!sources_.empty() && text.empty()) {
			bool more = sources_.back()(text);
			input_.insert(input_.end(), text.begin(), text.end());
			if (!more)
				sources_.pop_back();
		}
		if (input_state_.eof)
			nr = input_state_.range.length = input_.size() - ir;
		return !text.empty();
	}

	void reset() {
		input_state_ = {{0, input_.size()}, {1, 1}, true};
		program_state_ = {0, 0};
		parsing_ = false;
		reading_ = false;
		semantics_.clear();
	}

	const lug::grammar& grammar_;
	options options_;
	std::string input_;
	syntax_state input_state_;
	program_state program_state_;
	bool parsing_;
	bool reading_;
	std::vector<std::function<bool(std::string&)>> sources_;
	std::vector<stack_frame_type> stack_frames_;
	std::vector<std::tuple<program_state, syntax_state>> backtrack_stack_;
	std::vector<program_state> call_stack_;
	std::vector<std::tuple<program_state, program_state, syntax_state, syntax_state, unsigned short>> call_left_stack_;
	semantic_environment semantics_;
};

template <class InputIt, class = typename std::enable_if<std::is_same<char, typename std::iterator_traits<InputIt>::value_type>::value>::type>
inline bool parse(InputIt first, InputIt last, const grammar& g, const parser::options& options = parser::options{}) {
	return parser{g, options}.enqueue(first, last).parse();
}

inline bool parse(const std::string& t, const grammar& g, const parser::options& options = parser::options{}) {
	return parse(t.cbegin(), t.cend(), g, options);
}

inline bool parse(std::istream& s, const grammar& g, const parser::options& options = parser::options{}) {
	return parser{g, options}.push_source([&s](std::string& t) {
		if (std::getline(s, t)) {
			t.push_back('\n');
			return true;
		}
		return false;
	}).parse();
}

inline bool parse(const grammar& g, const parser::options& options = parser::options{}) {
	return parse(std::cin, g, options);
}

namespace lang
{

using lug::grammar;
using lug::rule;
using namespace expr::operators;
using namespace std::literals::string_literals;

} // namespace lang

} // namespace lug

#endif // LUG_HPP
