/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <unifex/bind_back.hpp>
#include <unifex/continuations.hpp>
#include <unifex/get_stop_token.hpp>
#include <unifex/manual_lifetime.hpp>
#include <unifex/manual_lifetime_union.hpp>
#include <unifex/receiver_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/std_concepts.hpp>
#include <unifex/type_list.hpp>
#include <unifex/type_traits.hpp>

#include <algorithm>
#include <tuple>
#include <type_traits>

#include <unifex/detail/prologue.hpp>

// There are reports of the asserts on cleanup_ firing when a let_value
// operation state is constructed in one shared libary and completed in another.
// The asserts fire because the addresses of two otherwise identical functions
// differ. If you have this problem, you can suppress the asserts by defining
// UNIFEX_DISABLE_LET_VALUE_CLEANUP_ASSERTS.
#if defined(UNIFEX_DISABLE_LET_VALUE_CLEANUP_ASSERTS)
#  define UNIFEX_ASSERT_CLEANUP(cond) ((void)0)
#else
#  define UNIFEX_ASSERT_CLEANUP(cond) UNIFEX_ASSERT(cond)
#endif

namespace unifex {
namespace _let_v {
template <typename... Values>
using decayed_tuple = std::tuple<std::decay_t<Values>...>;

template <typename Operation, typename... Values>
struct _successor_receiver {
  struct type;
};
template <typename Operation, typename... Values>
using successor_receiver =
    typename _successor_receiver<Operation, Values...>::type;

template <typename Operation, typename... Values>
struct _successor_receiver<Operation, Values...>::type {
  using successor_receiver = type;
  Operation& op_;

  typename Operation::receiver_type& get_receiver() const noexcept {
    return op_.receiver_;
  }

  template <typename... SuccessorValues>
  void set_value(SuccessorValues&&... values) && noexcept {
    UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == expectedCleanup);

    unifex::set_value(
        std::move(op_.receiver_), std::forward<SuccessorValues>(values)...);
  }

  void set_done() && noexcept {
    UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == expectedCleanup);

    unifex::set_done(std::move(op_.receiver_));
  }

  template <typename Error>
  void set_error(Error&& error) && noexcept {
    UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == expectedCleanup);

    unifex::set_error(std::move(op_.receiver_), std::forward<Error>(error));
  }

private:
  [[maybe_unused]] static constexpr void (*expectedCleanup)(
      Operation*) noexcept =
      Operation::template deactivateSuccOpAndDestructValues<Values...>;

  template <typename... Values2>
  using successor_operation =
      typename Operation::template successor_operation<Values2...>;

  template(typename CPO)                       //
      (requires is_receiver_query_cpo_v<CPO>)  //
      friend auto tag_invoke(CPO cpo, const successor_receiver& r) noexcept(
          std::is_nothrow_invocable_v<
              CPO,
              const typename Operation::receiver_type&>) -> std::
          invoke_result_t<CPO, const typename Operation::receiver_type&> {
    return std::move(cpo)(std::as_const(r.get_receiver()));
  }

#if UNIFEX_ENABLE_CONTINUATION_VISITATIONS
  template <typename Func>
  friend void tag_invoke(
      tag_t<visit_continuations>, const successor_receiver& r, Func&& f) {
    std::invoke(f, r.get_receiver());
  }
#endif
};

template <typename Operation>
struct _predecessor_receiver {
  struct type;
};
template <typename Operation>
using predecessor_receiver = typename _predecessor_receiver<Operation>::type;

template <typename Operation>
struct _predecessor_receiver<Operation>::type {
  using predecessor_receiver = type;
  using receiver_type = typename Operation::receiver_type;

  template <typename... Values>
  using successor_operation =
      typename Operation::template successor_operation<Values...>;

  template <typename... Values>
  using successor_type = typename Operation::template successor_type<Values...>;

  Operation& op_;

  receiver_type& get_receiver() const noexcept { return op_.receiver_; }

  template <typename... Values>
  void set_value(Values&&... values) && noexcept {
    auto& op = op_;
    UNIFEX_TRY {
      UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == op_.deactivatePredOp);
      // if we throw while constructing values_ then the default
      // cleanup_ will destroy predOp_
      auto& valueTuple =
          op.values_.template construct<decayed_tuple<Values...>>(
              std::forward<Values>(values)...);

      // ok, values_ initialized; next step is to construct the
      // successor operation, but we need to destroy predOp_ first
      // to make room
      //
      // leave a null function pointer in place while op is
      // temporarily in an invalid state; any accidental invocations
      // should be crashes intead of less-safe UB, and the compiler
      // ought to eliminate the dead store if it can prove it's dead
      std::exchange(op.cleanup_, nullptr)(&op);

      if constexpr (
          !is_nothrow_connectable_v<
              successor_type<Values...>,
              successor_receiver<Operation, Values...>> ||
          !noexcept(std::apply(std::move(op.func_), valueTuple))) {
        // setup a cleanup_ that will only destroy values_ in case
        // we throw while constructing succOp_
        op.cleanup_ = Operation::template destructValues<Values...>;
      }

      auto& succOp =
          unifex::activate_union_member_with<successor_operation<Values...>>(
              op.succOp_, [&] {
                static_assert(
                    noexcept(successor_receiver<Operation, Values...>{op}));
                return unifex::connect(
                    std::apply(std::move(op.func_), valueTuple),
                    successor_receiver<Operation, Values...>{op});
              });

      // now that succOp_ has been successfully constructed, the
      // op's cleanup_ needs to destroy both values_ and succOp_
      op.cleanup_ =
          Operation::template deactivateSuccOpAndDestructValues<Values...>;

      unifex::start(succOp);
    }
    UNIFEX_CATCH(...) {
      // depending on where the exception came from, cleanup_
      // could be any valid cleanup function
      UNIFEX_ASSERT_CLEANUP(op.cleanup_ != nullptr);
      unifex::set_error(std::move(op.receiver_), std::current_exception());
    }
  }

  void set_done() && noexcept {
    UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == op_.deactivatePredOp);
    unifex::set_done(std::move(op_.receiver_));
  }

  template <typename Error>
  void set_error(Error&& error) && noexcept {
    UNIFEX_ASSERT_CLEANUP(op_.cleanup_ == op_.deactivatePredOp);
    unifex::set_error(std::move(op_.receiver_), std::forward<Error>(error));
  }

  template(typename CPO)                       //
      (requires is_receiver_query_cpo_v<CPO>)  //
      friend auto tag_invoke(CPO cpo, const predecessor_receiver& r) noexcept(
          std::is_nothrow_invocable_v<CPO, const receiver_type&>)
          -> std::invoke_result_t<CPO, const receiver_type&> {
    return std::move(cpo)(std::as_const(r.get_receiver()));
  }

#if UNIFEX_ENABLE_CONTINUATION_VISITATIONS
  template <typename Func>
  friend void tag_invoke(
      tag_t<visit_continuations>, const predecessor_receiver& r, Func&& f) {
    std::invoke(f, r.get_receiver());
  }
#endif
};

template <typename Predecessor, typename SuccessorFactory, typename Receiver>
struct _op {
  struct type;
};
template <typename Predecessor, typename SuccessorFactory, typename Receiver>
using operation =
    typename _op<Predecessor, SuccessorFactory, remove_cvref_t<Receiver>>::type;

template <typename Predecessor, typename SuccessorFactory, typename Receiver>
struct _op<Predecessor, SuccessorFactory, Receiver>::type {
  using operation = type;
  using receiver_type = Receiver;

  template <typename... Values>
  using successor_type =
      std::invoke_result_t<SuccessorFactory, std::decay_t<Values>&...>;

  template <typename... Values>
  using successor_operation = connect_result_t<
      successor_type<Values...>,
      successor_receiver<operation, Values...>>;

  friend predecessor_receiver<operation>;
  template <typename Operation, typename... Values>
  friend struct _successor_receiver;

  template <typename SuccessorFactory2, typename Receiver2>
  explicit type(
      Predecessor&& pred, SuccessorFactory2&& func, Receiver2&& receiver)
    : func_((SuccessorFactory2&&)func)
    , receiver_((Receiver2&&)receiver) {
    unifex::activate_union_member_with(predOp_, [&] {
      return unifex::connect(
          (Predecessor&&)pred, predecessor_receiver<operation>{*this});
    });
  }

  ~type() { cleanup_(this); }

  void start() noexcept { unifex::start(predOp_.get()); }

private:
  static void deactivatePredOp(type* self) noexcept {
    unifex::deactivate_union_member(self->predOp_);
  }

  template <typename... Values>
  static void destructValues(type* self) noexcept {
    self->values_.template destruct<decayed_tuple<Values...>>();
  }

  template <typename... Values>
  static void deactivateSuccOpAndDestructValues(type* self) noexcept {
    unifex::deactivate_union_member<successor_operation<Values...>>(
        self->succOp_);
    self->values_.template destruct<decayed_tuple<Values...>>();
  }

  using predecessor_type = remove_cvref_t<Predecessor>;
  UNIFEX_NO_UNIQUE_ADDRESS SuccessorFactory func_;
  UNIFEX_NO_UNIQUE_ADDRESS Receiver receiver_;
  UNIFEX_NO_UNIQUE_ADDRESS typename sender_traits<predecessor_type>::
      template value_types<manual_lifetime_union, decayed_tuple>
          values_;
  union {
    manual_lifetime<
        connect_result_t<Predecessor, predecessor_receiver<operation>>>
        predOp_;
    typename sender_traits<predecessor_type>::
        template value_types<manual_lifetime_union, successor_operation>
            succOp_;
  };
  void (*cleanup_)(type*) noexcept = deactivatePredOp;
};

template <typename Predecessor, typename SuccessorFactory>
struct _sender {
  class type;
};
template <typename Predecessor, typename SuccessorFactory>
using sender = typename _sender<
    remove_cvref_t<Predecessor>,
    remove_cvref_t<SuccessorFactory>>::type;

template <typename Sender>
struct sends_done_impl
  : std::bool_constant<sender_traits<Sender>::sends_done> {};

template <typename... Successors>
using any_sends_done = std::disjunction<sends_done_impl<Successors>...>;

template <typename... Senders>
using all_always_scheduler_affine =
    std::conjunction<detail::_is_always_scheduler_affine<Senders>...>;

template <typename First, typename... Rest>
struct max_blocking_kind {
  constexpr _block::_enum operator()() const noexcept {
    _block::_enum enums[]{
        sender_traits<First>::blocking, sender_traits<Rest>::blocking...};
    return *std::max_element(std::begin(enums), std::end(enums));
  }
};

template <typename Predecessor, typename SuccessorFactory>
class _sender<Predecessor, SuccessorFactory>::type {
  using sender = type;
  Predecessor pred_;
  SuccessorFactory func_;
  instruction_ptr returnAddress_;

  template <typename... Values>
  using successor_type =
      std::invoke_result_t<SuccessorFactory, std::decay_t<Values>&...>;

  template <template <typename...> class List>
  using successor_types =
      sender_value_types_t<Predecessor, List, successor_type>;

  template <
      template <typename...> class Variant,
      template <typename...> class Tuple>
  struct value_types_impl {
    template <typename... Senders>
    using apply = typename concat_type_lists_unique_t<
        sender_value_types_t<Senders, type_list, Tuple>...>::
        template apply<Variant>;
  };

  // TODO: Ideally we'd only conditionally add the std::exception_ptr type
  // to the list of error types if it's possible that one of the following
  // operations is potentially throwing.
  //
  // Need to check whether any of the following bits are potentially-throwing:
  // - the construction of the value copies
  // - the invocation of the successor factory
  // - the invocation of the 'connect()' operation for the receiver.
  //
  // Unfortunately, we can't really check this last point reliably until we
  // know the concrete receiver type. So for now we conseratively report that
  // we might output std::exception_ptr.

  template <template <typename...> class Variant>
  struct error_types_impl {
    template <typename... Senders>
    using apply = typename concat_type_lists_unique_t<
        sender_error_types_t<Senders, type_list>...,
        type_list<std::exception_ptr>>::template apply<Variant>;
  };

public:
  template <
      template <typename...> class Variant,
      template <typename...> class Tuple>
  using value_types =
      successor_types<value_types_impl<Variant, Tuple>::template apply>;

  template <template <typename...> class Variant>
  using error_types =
      successor_types<error_types_impl<Variant>::template apply>;

  static constexpr bool sends_done = sender_traits<Predecessor>::sends_done ||
      successor_types<any_sends_done>::value;

  static constexpr blocking_kind blocking = std::max(
      sender_traits<Predecessor>::blocking(),
      std::min(
          successor_types<_let_v::max_blocking_kind>{}(),
          blocking_kind::maybe()));

  static constexpr bool is_always_scheduler_affine =
      sender_traits<Predecessor>::is_always_scheduler_affine &&
      successor_types<all_always_scheduler_affine>::value;

public:
  template <typename Predecessor2, typename SuccessorFactory2>
  explicit type(Predecessor2&& pred, SuccessorFactory2&& func, instruction_ptr returnAddress) noexcept(
      std::is_nothrow_constructible_v<Predecessor, Predecessor2> &&
      std::is_nothrow_constructible_v<SuccessorFactory, SuccessorFactory2>)
    : pred_((Predecessor2&&)pred)
    , func_((SuccessorFactory2&&)func)
    , returnAddress_(returnAddress) {}

  template(typename CPO, typename Sender, typename Receiver)  //
      (requires same_as<CPO, tag_t<unifex::connect>> AND same_as<
          remove_cvref_t<Sender>,
          type>)  //
      friend auto tag_invoke(
          [[maybe_unused]] CPO cpo, Sender&& sender, Receiver&& receiver)
          -> operation<
              decltype((static_cast<Sender&&>(sender).pred_)),
              SuccessorFactory,
              Receiver> {
    return operation<
        decltype((static_cast<Sender&&>(sender).pred_)),
        SuccessorFactory,
        Receiver>{
        static_cast<Sender&&>(sender).pred_,
        static_cast<Sender&&>(sender).func_,
        static_cast<Receiver&&>(receiver)};
  }

  friend constexpr blocking_kind
  tag_invoke(tag_t<unifex::blocking>, const type& sender) noexcept {
    // get the runtime blocking_kind for the predecessor
    blocking_kind pred = blocking(sender.pred_);
    // we have to go with the static result for the successors since we don't
    // know how pred_ will complete
    blocking_kind succ = successor_types<_let_v::max_blocking_kind>{}();

    return std::max(pred(), std::min(succ(), blocking_kind::maybe()));
  }

  friend instruction_ptr
  tag_invoke(tag_t<get_return_address>, const type& t) noexcept {
    return t.returnAddress_;
  }
};

namespace _cpo {
struct _fn {
private:
  struct _impl_fn {
    template(typename Predecessor, typename SuccessorFactory)         //
        (requires tag_invocable<_fn, Predecessor, SuccessorFactory>)  //
        auto
        operator()(Predecessor&& predecessor,
                   SuccessorFactory&& func,
                   instruction_ptr) const
        noexcept(is_nothrow_tag_invocable_v<_fn, Predecessor, SuccessorFactory>)
            -> tag_invoke_result_t<_fn, Predecessor, SuccessorFactory> {
      return unifex::tag_invoke(
          _fn{},
          std::forward<Predecessor>(predecessor),
          std::forward<SuccessorFactory>(func));
    }

    template(typename Predecessor, typename SuccessorFactory)           //
        (requires(!tag_invocable<_fn, Predecessor, SuccessorFactory>))  //
        auto
        operator()(Predecessor&& predecessor,
                   SuccessorFactory&& func,
                   instruction_ptr returnAddress) const
        noexcept(std::is_nothrow_constructible_v<
                 _let_v::sender<Predecessor, SuccessorFactory>,
                 Predecessor,
                 SuccessorFactory,
                 instruction_ptr>)
            -> _let_v::sender<Predecessor, SuccessorFactory> {
      return _let_v::sender<Predecessor, SuccessorFactory>{
          std::forward<Predecessor>(predecessor),
          std::forward<SuccessorFactory>(func),
          returnAddress};
    }
  };

public:
  template <typename Predecessor, typename SuccessorFactory>
  auto operator()(Predecessor&& pred, SuccessorFactory&& func) const
      noexcept(std::is_nothrow_invocable_v<
               _impl_fn,
               Predecessor,
               SuccessorFactory,
               instruction_ptr>)
          -> std::invoke_result_t<
              _impl_fn,
              Predecessor,
              SuccessorFactory,
              instruction_ptr> {
    return _impl_fn{}(
        std::forward<Predecessor>(pred),
        std::forward<SuccessorFactory>(func),
        UNIFEX_READ_RETURN_ADDRESS());
  }
  template <typename SuccessorFactory>
  constexpr auto operator()(SuccessorFactory&& func) const
      noexcept(std::is_nothrow_invocable_v<
               tag_t<bind_back>,
               _impl_fn,
               SuccessorFactory,
               instruction_ptr>)
          -> bind_back_result_t<_impl_fn, SuccessorFactory, instruction_ptr> {
    return bind_back(
        _impl_fn{},
        std::forward<SuccessorFactory>(func),
        UNIFEX_READ_RETURN_ADDRESS());
  }
};
}  // namespace _cpo
}  // namespace _let_v

inline constexpr _let_v::_cpo::_fn let_value{};

}  // namespace unifex

#undef UNIFEX_ASSERT_CLEANUP

#include <unifex/detail/epilogue.hpp>
