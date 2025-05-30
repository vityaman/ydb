#pragma once

#include "defs.h"
#include "actorid.h"
#ifdef USE_ACTOR_CALLSTACK
#include "callstack.h"
#endif
#include "event_load.h"

#include <ydb/library/actors/wilson/wilson_trace.h>

#include <util/system/hp_timer.h>
#include <util/generic/maybe.h>

namespace NActors {
    class TChunkSerializer;
    class IActor;

    class IEventBase
        : TNonCopyable {
    public:
        // actual typing is performed by IEventHandle

        virtual ~IEventBase() {
        }

        virtual TString ToStringHeader() const = 0;
        virtual TString ToString() const {
            return ToStringHeader();
        }
        virtual ui32 CalculateSerializedSize() const {
            return 0;
        }
        virtual ui32 Type() const = 0;
        virtual bool SerializeToArcadiaStream(TChunkSerializer*) const = 0;
        virtual bool IsSerializable() const = 0;
        virtual ui32 CalculateSerializedSizeCached() const {
            return CalculateSerializedSize();
        }
        virtual TEventSerializationInfo CreateSerializationInfo() const { return {}; }
    };

    template <typename TEventType>
    class TEventHandle;

    template <typename TEvent>
    concept TEventWithLoadSupport = requires {
        TEvent::Load;
    };

    // fat handle
    class IEventHandle : TNonCopyable {
        struct TOnNondelivery {
            TActorId Recipient;

            TOnNondelivery(const TActorId& recipient)
                : Recipient(recipient)
            {
            }
        };

        static const TEventSerializedData EmptyBuffer;

    public:
        typedef TAutoPtr<IEventHandle> TPtr;

    public:
        // Used by a mailbox intrusive list
        std::atomic<uintptr_t> NextLinkPtr;

    public:
        template <typename TEv>
        inline TEv* CastAsLocal() const noexcept {
            auto fits = GetTypeRewrite() == TEv::EventType;

            return fits ? static_cast<TEv*>(Event.Get()) : nullptr;
        }

        template <typename TEv>
        inline TEv* StaticCastAsLocal() const noexcept {  // blind cast
            return static_cast<TEv*>(Event.Get());
        }

        template <typename TEv>
        static TAutoPtr<TEventHandle<TEv>> Downcast(TAutoPtr<IEventHandle>&& ev) {
            Y_DEBUG_ABORT_UNLESS(ev->GetTypeRewrite() == TEv::EventType);

            return static_cast<typename TEv::THandle*>(ev.Release());
        };

        template <typename TEv>
        static TAutoPtr<IEventHandle> Upcast(TAutoPtr<TEventHandle<TEv>>&& ev) {
            return ev.Release();
        };

        template <typename TEventType>
        TEventType* Get() {
            Y_ENSURE(Type == TEventType::EventType,
                "Event type " << Type << " doesn't match the expected type " << TEventType::EventType
                << " class " << TypeName<TEventType>());

            if (!Event) {
                if constexpr (TEventWithLoadSupport<TEventType>) {
                    // Note: we require Load to return the correct derived type
                    // This makes sure static_cast below is always type-safe
                    TEventType* loaded = TEventType::Load(Buffer ? Buffer.Get() : &EmptyBuffer);
                    Event.Reset(loaded);
                    Buffer.Reset();
                } else {
                    Y_ENSURE(false, "Event type " << Type << " cannot be loaded by class " << TypeName<TEventType>());
                }
            }

            if (Event) {
                return static_cast<TEventType*>(Event.Get());
            }

            Y_ENSURE(false, "Failed to Load() event type " << Type << " class " << TypeName<TEventType>());
        }

        template <typename T>
        TAutoPtr<T> Release() {
            TAutoPtr<T> x = Get<T>();
            Y_UNUSED(Event.Release());
            Buffer.Reset();
            return x;
        }

        enum EFlags: ui32 {
            FlagTrackDelivery = 1 << 0,
            FlagForwardOnNondelivery = 1 << 1,
            FlagSubscribeOnSession = 1 << 2,
            FlagUseSubChannel = 1 << 3,
            FlagGenerateUnsureUndelivered = 1 << 4,
            FlagExtendedFormat = 1 << 5,
            FlagDebugTrackReceive = 1 << 6,
        };
        using TEventFlags = ui32;

        const ui32 Type;
        const TEventFlags Flags;
        const TActorId Recipient;
        TActorId Sender;
        const ui64 Cookie;
        const TScopeId OriginScopeId = TScopeId::LocallyGenerated; // filled in when the message is received from Interconnect

        // if set, used by ActorSystem/Interconnect to report tracepoints
        NWilson::TTraceId TraceId;

        // filled if feeded by interconnect session
        const TActorId InterconnectSession;

#ifdef ACTORSLIB_COLLECT_EXEC_STATS
        ::NHPTimer::STime SendTime;
#endif

        static const size_t ChannelBits = 12;
        static const size_t ChannelShift = (sizeof(ui32) << 3) - ChannelBits;

#ifdef USE_ACTOR_CALLSTACK
        TCallstack Callstack;
#endif
        ui16 GetChannel() const noexcept {
            return Flags >> ChannelShift;
        }

        ui64 GetSubChannel() const noexcept {
            return Flags & FlagUseSubChannel ? Sender.LocalId() : 0ULL;
        }

        static ui32 MakeFlags(ui32 channel, TEventFlags flags) {
            Y_ENSURE(channel < (1 << ChannelBits));
            Y_ENSURE(flags < (1 << ChannelShift));
            return (flags | (channel << ChannelShift));
        }

    private:
        THolder<IEventBase> Event;
        TIntrusivePtr<TEventSerializedData> Buffer;

        TActorId RewriteRecipient;
        ui32 RewriteType;

        THolder<TOnNondelivery> OnNondeliveryHolder; // only for local events

    public:
        void Rewrite(ui32 typeRewrite, TActorId recipientRewrite) {
            RewriteRecipient = recipientRewrite;
            RewriteType = typeRewrite;
        }

        void DropRewrite() {
            RewriteRecipient = Recipient;
            RewriteType = Type;
        }

        const TActorId& GetRecipientRewrite() const {
            return RewriteRecipient;
        }

        ui32 GetTypeRewrite() const {
            return RewriteType;
        }

        TActorId GetForwardOnNondeliveryRecipient() const {
            return OnNondeliveryHolder.Get() ? OnNondeliveryHolder->Recipient : TActorId();
        }

#ifndef NDEBUG
        static inline thread_local bool TrackNextEvent = false;

        /**
         * Call this function in gdb before
         * sending the event you want to debug
         * and continue execution. __builtin_debugtrap/SIGTRAP
         * will stop gdb at the receiving point.
         * Currently, to get to Handle function you
         * also need to ascend couple frames (up, up) and step to
         * function you are searching for
         */
        static void DoTrackNextEvent();

        static TEventFlags ApplyGlobals(TEventFlags flags) {
            bool trackNextEvent = std::exchange(TrackNextEvent, false);
            return flags | (trackNextEvent ? FlagDebugTrackReceive : 0);
        }
#else
        Y_FORCE_INLINE static TEventFlags ApplyGlobals(TEventFlags flags) {
            return flags;
        }
#endif

        IEventHandle(const TActorId& recipient, const TActorId& sender, IEventBase* ev, TEventFlags flags = 0, ui64 cookie = 0,
                     const TActorId* forwardOnNondelivery = nullptr, NWilson::TTraceId traceId = {})
            : Type(ev->Type())
            , Flags(ApplyGlobals(flags))
            , Recipient(recipient)
            , Sender(sender)
            , Cookie(cookie)
            , TraceId(std::move(traceId))
#ifdef ACTORSLIB_COLLECT_EXEC_STATS
            , SendTime(0)
#endif
            , Event(ev)
            , RewriteRecipient(Recipient)
            , RewriteType(Type)
        {
            if (forwardOnNondelivery)
                OnNondeliveryHolder.Reset(new TOnNondelivery(*forwardOnNondelivery));
        }

        IEventHandle(ui32 type,
                     TEventFlags flags,
                     const TActorId& recipient,
                     const TActorId& sender,
                     TIntrusivePtr<TEventSerializedData> buffer,
                     ui64 cookie,
                     const TActorId* forwardOnNondelivery = nullptr,
                     NWilson::TTraceId traceId = {})
            : Type(type)
            , Flags(ApplyGlobals(flags))
            , Recipient(recipient)
            , Sender(sender)
            , Cookie(cookie)
            , TraceId(std::move(traceId))
#ifdef ACTORSLIB_COLLECT_EXEC_STATS
            , SendTime(0)
#endif
            , Buffer(std::move(buffer))
            , RewriteRecipient(Recipient)
            , RewriteType(Type)
        {
            if (forwardOnNondelivery)
                OnNondeliveryHolder.Reset(new TOnNondelivery(*forwardOnNondelivery));
        }

        // Special ctor for events from interconnect.
        IEventHandle(const TActorId& session,
                     ui32 type,
                     TEventFlags flags,
                     const TActorId& recipient,
                     const TActorId& sender,
                     TIntrusivePtr<TEventSerializedData> buffer,
                     ui64 cookie,
                     TScopeId originScopeId,
                     NWilson::TTraceId traceId) noexcept
            : Type(type)
            , Flags(ApplyGlobals(flags))
            , Recipient(recipient)
            , Sender(sender)
            , Cookie(cookie)
            , OriginScopeId(originScopeId)
            , TraceId(std::move(traceId))
            , InterconnectSession(session)
#ifdef ACTORSLIB_COLLECT_EXEC_STATS
            , SendTime(0)
#endif
            , Buffer(std::move(buffer))
            , RewriteRecipient(Recipient)
            , RewriteType(Type)
        {
        }

        TIntrusivePtr<TEventSerializedData> GetChainBuffer();
        TIntrusivePtr<TEventSerializedData> ReleaseChainBuffer();

        ui32 GetSize() const {
            if (Buffer) {
                return Buffer->GetSize();
            } else if (Event) {
                return Event->CalculateSerializedSize();
            } else {
                return 0;
            }
        }

        bool HasBuffer() const {
            return bool(Buffer);
        }

        bool HasEvent() const {
            return bool(Event);
        }

        IEventBase* GetBase() {
            if (!Event) {
                if (!Buffer)
                    return nullptr;
                else
                    ythrow TWithBackTrace<yexception>() << "don't know how to load the event from buffer";
            }

            return Event.Get();
        }

        TAutoPtr<IEventBase> ReleaseBase() {
            TAutoPtr<IEventBase> x = GetBase();
            Y_UNUSED(Event.Release());
            Buffer.Reset();
            return x;
        }

        TAutoPtr<IEventHandle> Forward(const TActorId& dest) {
            if (Event)
                return new IEventHandle(dest, Sender, Event.Release(), Flags, Cookie, nullptr, std::move(TraceId));
            else
                return new IEventHandle(Type, Flags, dest, Sender, Buffer, Cookie, nullptr, std::move(TraceId));
        }

        TString GetTypeName() const;
        TString ToString() const;

        [[nodiscard]] static std::unique_ptr<IEventHandle> Forward(std::unique_ptr<IEventHandle>&& ev, TActorId recipient);
        [[nodiscard]] static std::unique_ptr<IEventHandle> ForwardOnNondelivery(std::unique_ptr<IEventHandle>&& ev, ui32 reason, bool unsure = false);

        [[nodiscard]] static TAutoPtr<IEventHandle> Forward(TAutoPtr<IEventHandle>&& ev, TActorId recipient) {
            return Forward(std::unique_ptr<IEventHandle>(ev.Release()), recipient).release();
        }

        [[nodiscard]] static THolder<IEventHandle> Forward(THolder<IEventHandle>&& ev, TActorId recipient) {
            return THolder(Forward(std::unique_ptr<IEventHandle>(ev.Release()), recipient).release());
        }

        [[nodiscard]] static TAutoPtr<IEventHandle> ForwardOnNondelivery(TAutoPtr<IEventHandle>&& ev, ui32 reason, bool unsure = false) {
            return ForwardOnNondelivery(std::unique_ptr<IEventHandle>(ev.Release()), reason, unsure).release();
        }

        [[nodiscard]] static THolder<IEventHandle> ForwardOnNondelivery(THolder<IEventHandle>&& ev, ui32 reason, bool unsure = false) {
            return THolder(ForwardOnNondelivery(std::unique_ptr<IEventHandle>(ev.Release()), reason, unsure).release());
        }

        template<typename T>
        static TAutoPtr<T> Release(TAutoPtr<IEventHandle>& ev) {
            return ev->Release<T>();
        }

        template<typename T>
        static TAutoPtr<T> Release(THolder<IEventHandle>& ev) {
            return ev->Release<T>();
        }
    };

    template <typename TEventType>
    class TEventHandle: public IEventHandle {
        TEventHandle(); // we never made instance of TEventHandle

    public:
        typedef TAutoPtr<TEventHandle<TEventType>> TPtr;

    public:
        TEventType* Get() {
            return IEventHandle::Get<TEventType>();
        }

        TAutoPtr<TEventType> Release() {
            return IEventHandle::Release<TEventType>();
        }
    };

    static_assert(sizeof(TEventHandle<IEventBase>) == sizeof(IEventHandle), "expect sizeof(TEventHandle<IEventBase>) == sizeof(IEventHandle)");

    template <typename TEventType, ui32 EventType0>
    class TEventBase: public IEventBase {
    public:
        static constexpr ui32 EventType = EventType0;
        ui32 Type() const override {
            return EventType0;
        }
        // still abstract

        typedef TEventHandle<TEventType> THandle;
        typedef typename THandle::TPtr TPtr;
    };

#define DEFINE_SIMPLE_LOCAL_EVENT(eventType, header)                    \
    TString ToStringHeader() const override {                           \
        return TString(header);                                         \
    }                                                                   \
    bool SerializeToArcadiaStream(NActors::TChunkSerializer*) const override { \
        Y_ABORT("Local event " #eventType " is not serializable");       \
    }                                                                   \
    bool IsSerializable() const override {                              \
        return false;                                                   \
    }

#define DEFINE_SIMPLE_NONLOCAL_EVENT(eventType, header)                 \
    TString ToStringHeader() const override {                           \
        return TString(header);                                         \
    }                                                                   \
    bool SerializeToArcadiaStream(NActors::TChunkSerializer*) const override { \
        return true;                                                    \
    }                                                                   \
    static eventType* Load(const NActors::TEventSerializedData*) {      \
        return new eventType();                                         \
    }                                                                   \
    bool IsSerializable() const override {                              \
        return true;                                                    \
    }
}
