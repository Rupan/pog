#pragma once

#include <memory>
#include <vector>

#include <re2/set.h>

#include <pog/grammar.h>
#include <pog/token.h>

namespace pog {

template <typename ValueT>
struct TokenMatch
{
	TokenMatch(const Symbol<ValueT>* sym) : symbol(sym), value(), match_length(0) {}
	template <typename T>
	TokenMatch(const Symbol<ValueT>* sym, T&& v, std::size_t len) : symbol(sym), value(std::forward<T>(v)), match_length(len) {}
	TokenMatch(const TokenMatch&) = default;
	TokenMatch(TokenMatch&&) noexcept = default;

	TokenMatch& operator=(const TokenMatch&) = default;
	TokenMatch& operator=(TokenMatch&&) noexcept = default;

	const Symbol<ValueT>* symbol;
	ValueT value;
	std::size_t match_length;
};

struct InputStream
{
	std::string content;
	re2::StringPiece stream;
	bool at_end;
};

template <typename ValueT>
struct StateInfo
{
	std::string name;
	std::unique_ptr<re2::RE2::Set> re_set;
	std::vector<Token<ValueT>*> tokens;
};

template <typename ValueT>
class Tokenizer
{
public:
	static constexpr std::string_view DefaultState = "@default";

	using GrammarType = Grammar<ValueT>;
	using StateInfoType = StateInfo<ValueT>;
	using SymbolType = Symbol<ValueT>;
	using TokenType = Token<ValueT>;
	using TokenMatchType = TokenMatch<ValueT>;

	Tokenizer(const GrammarType* grammar) : _grammar(grammar), _tokens(), _state_info(), _input_stack(), _current_state(nullptr)
	{
		_current_state = get_or_make_state_info(std::string{DefaultState});
		add_token("$", _grammar->get_end_of_input_symbol(), std::vector<std::string>{std::string{DefaultState}});
	}

	void prepare()
	{
		for (auto&& [name, info] : _state_info)
			info.re_set->Compile();
	}

	TokenType* get_end_token() const
	{
		return _tokens[0].get();
	}

	TokenType* add_token(const std::string& pattern, const SymbolType* symbol, const std::vector<std::string>& states)
	{
		_tokens.push_back(std::make_unique<TokenType>(_tokens.size(), pattern, symbol));
		for (const auto& state : states)
		{
			std::string error;
			auto* state_info = get_or_make_state_info(state);
			state_info->re_set->Add(_tokens.back()->get_pattern(), &error);
			state_info->tokens.push_back(_tokens.back().get());
			assert(error.empty() && "Error when compiling token regexp");
		}
		return _tokens.back().get();
	}

	void push_input_stream(std::istream& stream)
	{
		std::string input;
		std::vector<char> block(4096);
		while (stream.good())
		{
			stream.read(block.data(), block.size());
			input.append(std::string_view(block.data(), stream.gcount()));
		}

		_input_stack.emplace_back(InputStream{std::move(input), re2::StringPiece{}, false});
		_input_stack.back().stream = re2::StringPiece{_input_stack.back().content};
	}

	void pop_input_stream()
	{
		_input_stack.pop_back();
	}

	std::optional<TokenMatchType> next_token()
	{
		bool repeat = true;
		while (repeat)
		{
			if (_input_stack.empty())
				return std::nullopt;

			auto& current_input = _input_stack.back();
			if (!current_input.at_end)
			{
				// TODO: If consume_matched_token was not called we should remember the token somewhere so we don't unnecessarily match the input
				// once again for the same token
				std::vector<int> matched_patterns;
				_current_state->re_set->Match(current_input.stream, &matched_patterns);

				// Haven't matched anything, tokenization failure, we will get into endless loop
				if (matched_patterns.empty())
					return std::nullopt;

				re2::StringPiece submatch;
				const TokenType* best_match = nullptr;
				int longest_match = -1;
				for (auto pattern_index : matched_patterns)
				{
					_current_state->tokens[pattern_index]->get_regexp()->Match(current_input.stream, 0, current_input.stream.size(), re2::RE2::Anchor::ANCHOR_START, &submatch, 1);
					if (longest_match < static_cast<int>(submatch.size()))
					{
						best_match = _current_state->tokens[pattern_index];
						longest_match = static_cast<int>(submatch.size());
					}
				}

				if (current_input.stream.size() == 0)
					current_input.at_end = true;

				if (best_match->has_transition_to_state())
					enter_state(best_match->get_transition_to_state());

				ValueT value{};
				if (best_match->has_action())
					value = best_match->perform_action(std::string_view{current_input.stream.data(), static_cast<std::size_t>(longest_match)});

				current_input.stream.remove_prefix(longest_match);
				if (!best_match->has_symbol())
					continue;

				return TokenMatchType{best_match->get_symbol(), std::move(value), static_cast<std::size_t>(longest_match)};
			}

			// Fallback just in case that we've reached end of input and it didn't get propagated to parser
			// If user doesn't have symbol associated with token then we don't return in above continue, but rather loop
			// If we've reached end in the meantime then we fall through here
			return _grammar->get_end_of_input_symbol();
		}

		return std::nullopt;
	}

private:
	StateInfoType* get_or_make_state_info(const std::string& name)
	{
		auto itr = _state_info.find(name);
		if (itr == _state_info.end())
			std::tie(itr, std::ignore) = _state_info.emplace(name, StateInfoType{
				name,
				std::make_unique<re2::RE2::Set>(re2::RE2::DefaultOptions, re2::RE2::Anchor::ANCHOR_START),
				std::vector<TokenType*>{}
			});
		return &itr->second;
	}

	StateInfoType* get_state_info(const std::string& name)
	{
		auto itr = _state_info.find(name);
		if (itr == _state_info.end())
			return nullptr;
		return &itr->second;
	}

	void enter_state(const std::string& state)
	{
		_current_state = get_state_info(state);
		assert(_current_state && "Transition to unknown state in tokenizer");
	}

	const GrammarType* _grammar;
	std::vector<std::unique_ptr<TokenType>> _tokens;

	std::unordered_map<std::string, StateInfoType> _state_info;
	std::vector<InputStream> _input_stack;
	StateInfoType* _current_state;
};

} // namespace pog
