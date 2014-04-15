#pragma once

#include <ntddk.h>
#include <Ntstrsafe.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

class CWdmSpinLock
{
public:
    CWdmSpinLock()
    { KeInitializeSpinLock(&m_Lock); }
    void Lock()
    { KeAcquireSpinLock(&m_Lock, &m_OldIrql); }
    void Unlock()
    { KeReleaseSpinLock(&m_Lock, m_OldIrql); }
private:
    KSPIN_LOCK m_Lock;
    KIRQL m_OldIrql;
};

template <typename T>
class CLockedContext
{
public:
    CLockedContext(T &LockObject)
        : m_LockObject(LockObject)
    { m_LockObject.Lock(); }

    ~CLockedContext()
    { m_LockObject.Unlock(); }

private:
    T &m_LockObject;

    CLockedContext(const CLockedContext&) = delete;
    CLockedContext& operator= (const CLockedContext&) = delete;
};

typedef CLockedContext<CWdmSpinLock> TSpinLocker;

class CWdmRefCounter
{
public:
    void AddRef() { InterlockedIncrement(&m_Counter); }
    void AddRef(LONG RefCnt) { InterlockedAdd(&m_Counter, RefCnt); }
    LONG Release() { return InterlockedDecrement(&m_Counter); }
    LONG Release(LONG RefCnt) { AddRef(-RefCnt); }
    operator LONG () { return m_Counter; }
private:
    LONG m_Counter = 0;
};

class CLockedAccess
{
public:
    void Lock() { m_Lock.Lock(); }
    void Unlock() { m_Lock.Unlock(); }
private:
    CWdmSpinLock m_Lock;
};

class CRawAccess
{
public:
    void Lock() { }
    void Unlock() { }
};

class CCountingObject
{
public:
    void CounterIncrement() { m_Counter++; }
    void CounterDecrement() { m_Counter--; }
    ULONG GetCount() { return m_Counter; }
private:
    ULONG m_Counter = 0;
};

class CNonCountingObject
{
public:
    void CounterIncrement() { }
    void CounterDecrement() { }
protected:
    ULONG GetCount() { return 0; }
};

template <typename TEntryType, typename TAccessStrategy, typename TCountingStrategy>
class CWdmList : private TAccessStrategy, public TCountingStrategy
{
public:
    CWdmList()
    { InitializeListHead(&m_List); }

    ~CWdmList()
    { Clear(); }

    void Clear()
    { ForEachDetached([](TEntryType* Entry) { delete Entry; return true; }); }

    bool IsEmpty()
    { return IsListEmpty(&m_List) ? true : false; }

    TEntryType *Pop()
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        return Pop_LockLess();
    }

    ULONG Push(TEntryType *Entry)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        InsertHeadList(&m_List, Entry->GetListEntry());
        CounterIncrement();
        return GetCount();
    }

    ULONG PushBack(TEntryType *Entry)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        InsertTailList(&m_List, Entry->GetListEntry());
        CounterIncrement();
        return GetCount();
    }

    void Remove(TEntryType *Entry)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        Remove_LockLess(Entry->GetListEntry());
    }

    template <typename TFunctor>
    bool ForEachDetached(TFunctor Functor)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        while (!IsListEmpty(&m_List))
        {
            if (!Functor(Pop_LockLess()))
            {
                return false;
            }
        }
        return true;
    }

    template <typename TPredicate, typename TFunctor>
    bool ForEachDetachedIf(TPredicate Predicate, TFunctor Functor)
    {
        return ForEachPrepareIf(Predicate, [this](PLIST_ENTRY Entry){ Remove_LockLess(Entry); }, Functor);
    }

    template <typename TFunctor>
    bool ForEach(TFunctor Functor)
    {
        return ForEachPrepareIf([](TEntryType*) { return true; }, [](PLIST_ENTRY){}, Functor);
    }

    template <typename TPredicate, typename TFunctor>
    bool ForEachIf(TPredicate Predicate, TFunctor Functor)
    {
        return ForEachPrepareIf(Predicate, [](PLIST_ENTRY){}, Functor);
    }

private:
    template <typename TPredicate, typename TPrepareFunctor, typename TFunctor>
    bool ForEachPrepareIf(TPredicate Predicate, TPrepareFunctor Prepare, TFunctor Functor)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);

        PLIST_ENTRY NextEntry = nullptr;

        for (auto CurrEntry = m_List.Flink; CurrEntry != &m_List; CurrEntry = NextEntry)
        {
            NextEntry = CurrEntry->Flink;
            auto Object = TEntryType::GetByListEntry(CurrEntry);

            if (Predicate(Object))
            {
                Prepare(CurrEntry);
                if (!Functor(Object))
                {
                    return false;
                }
            }
        }

        return true;
    }

    TEntryType *Pop_LockLess()
    {
        CounterDecrement();
        return TEntryType::GetByListEntry(RemoveHeadList(&m_List));
    }

    void Remove_LockLess(PLIST_ENTRY Entry)
    {
        RemoveEntryList(Entry);
        CounterDecrement();
    }

    LIST_ENTRY m_List;
    ULONG m_NumEntries = 0;
};

static inline bool ConstTrue(...) { return true; }
static inline bool ConstFalse(...) { return false; }

template <typename TEntryType, typename TAccessStrategy, typename TCountingStrategy>
class CWdmSet : private TAccessStrategy, public TCountingStrategy
{
public:
    bool Add(TEntryType *NewEntry)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        if (!Contains_LockLess(NewEntry))
        {
            m_Objects.PushBack(NewEntry);
            CounterIncrement();
            return true;
        }

        return false;
    }

    template <typename TEntryId>
    bool Delete(TEntryId *Id)
    {
        auto Removed = false;
        CLockedContext<TAccessStrategy> LockedContext(*this);

        m_Objects.ForEachDetachedIf([Id](TEntryType *ExistingEntry) { return *ExistingEntry == *Id; },
                                    [this, &Removed](TEntryType *ExistingEntry)
                                    {
                                            delete ExistingEntry;
                                            CounterDecrement();
                                            Removed = true;
                                            return false;
                                    });

        return Removed;
    }

    void Dump()
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        m_Objects.ForEach([](TEntryType *Entry) { Entry->Dump(); return true; });
    }

    template <typename TEntryId>
    bool Contains(TEntryId *Id)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        return Contains_LockLess(Id);
    }

    template <typename TEntryId, typename TModifier>
    bool ModifyOne(TEntryId *Id, TModifier ModifierFunc)
    {
        CLockedContext<TAccessStrategy> LockedContext(*this);
        return !m_Objects.ForEachIf([Id](TEntryType *ExistingEntry) { return *ExistingEntry == *Id; },
                                    [&ModifierFunc](TEntryType *Entry) { ModifierFunc(Entry); return false; });
    }

private:
    template <typename TEntryId>
    bool Contains_LockLess(TEntryId *Id)
    {
        auto MatchFound = false;

        m_Objects.ForEachIf([Id](TEntryType *ExistingEntry) { return *ExistingEntry == *Id; },
                            [&MatchFound](TEntryType *) { MatchFound = true; return false; });

        return MatchFound;
    }


    CWdmList<TEntryType, CRawAccess, CNonCountingObject> m_Objects;
};

class CWdmEvent : public CAllocatable<NonPagedPool, 'VEHR'>
{
public:
    CWdmEvent(EVENT_TYPE Type = SynchronizationEvent, BOOLEAN InitialState = FALSE)
    { KeInitializeEvent(&m_Event, Type, InitialState); };

    NTSTATUS Wait(bool WithTimeout = false, LONGLONG Timeout = 0, bool Alertable = false);

    bool Set(KPRIORITY Increment = IO_NO_INCREMENT, bool Wait = false)
    { return KeSetEvent(&m_Event, Increment, Wait ? TRUE : FALSE) ? true : false; }
    void Clear()
    { KeClearEvent(&m_Event); }
    bool Reset()
    { return KeResetEvent(&m_Event) ? true : false; }

    operator PKEVENT () { return &m_Event; }

    CWdmEvent(const CWdmEvent&) = delete;
    CWdmEvent& operator= (const CWdmEvent&) = delete;
private:
    KEVENT m_Event;
};

class CStringBase
{
public:
    bool operator== (const CStringBase &Other)
    { return *this == Other.m_String; }

    bool operator== (const UNICODE_STRING& Str);

    bool operator== (PCWSTR Other)
    {
        UNICODE_STRING str;
        if (NT_SUCCESS(RtlUnicodeStringInit(&str, Other)))
        {
            return *this == str;
        }
        return false;
    }

    operator PCUNICODE_STRING() const { return &m_String; };

    NTSTATUS ToString(ULONG Val, ULONG Base)
    { return RtlIntegerToUnicodeString(Val, Base, &m_String); }

protected:
    CStringBase(const CStringBase&) = delete;
    CStringBase& operator= (const CStringBase&) = delete;
    CStringBase() {};
    ~CStringBase() {};

    UNICODE_STRING m_String;
};
class CStringHolder : public CStringBase
{
public:
    NTSTATUS Attach(NTSTRSAFE_PCWSTR String)
    { return RtlUnicodeStringInit(&m_String, String); }

    NTSTATUS Attach(NTSTRSAFE_PCWSTR String, USHORT SizeInBytes)
    {
        m_String.Length = SizeInBytes;
        m_String.MaximumLength = SizeInBytes;
        m_String.Buffer = const_cast<PWCH>(String);
        return RtlUnicodeStringValidate(&m_String);
    }

    //This initialization may be done in-class without
    //constructor definition but MS compiler crashes with internal error
    CStringHolder()
    { m_String = {}; }

private:
    CStringHolder(const CStringHolder&) = delete;
    CStringHolder& operator= (const CStringHolder&) = delete;
};

class CString : public CStringBase
{
public:
    NTSTATUS Create(NTSTRSAFE_PCWSTR String);
    NTSTATUS Create(NTSTRSAFE_PCWSTR Prefix, ULONG Postfix);
    NTSTATUS Append(PCUNICODE_STRING String);
    NTSTATUS Append(ULONG Num, ULONG Base = 10);
    void Destroy();

    //This initialization may be done in-class without
    //constructor definition but MS compiler crashes with internal error
    CString()
    { m_String = {}; }

    ~CString()
    { Destroy(); }

private:

    NTSTATUS Resize(USHORT NewLenBytes);

    CString(const CString&) = delete;
    CString& operator= (const CString&) = delete;
};

PVOID DuplicateStaticBuffer(const void *Buffer, SIZE_T Length, POOL_TYPE PoolType = PagedPool);

template<typename T>
class CInstanceCounter
{
public:
    CInstanceCounter()
    {
        static LONG volatile Counter = 0;
        m_Number = InterlockedIncrement(&Counter);
    }

    operator ULONG() const { return static_cast<ULONG>(m_Number); };

private:
    LONG m_Number = 0;

    CInstanceCounter(const CInstanceCounter&) = delete;
    CInstanceCounter& operator= (const CInstanceCounter&) = delete;
};
