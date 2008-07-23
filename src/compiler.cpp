/* Copyright (c) 2008, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "compiler.h"
#include "assembler.h"

using namespace vm;

namespace {

const bool DebugAppend = false;
const bool DebugCompile = false;
const bool DebugStack = false;
const bool DebugRegisters = false;

class Context;
class Value;
class Stack;
class Site;
class RegisterSite;
class Event;
class PushEvent;
class Read;

void NO_RETURN abort(Context*);

void
apply(Context* c, UnaryOperation op, unsigned size, Site* a);

void
apply(Context* c, BinaryOperation op, unsigned size, Site* a, Site* b);

enum ConstantCompare {
  CompareNone,
  CompareLess,
  CompareGreater,
  CompareEqual
};

class Site {
 public:
  Site(): next(0) { }
  
  virtual ~Site() { }

  virtual Site* readTarget(Context*, Read*) { return this; }

  virtual unsigned copyCost(Context*, Site*) = 0;
  
  virtual void acquire(Context*, Stack*, Value**, unsigned, Value*) { }

  virtual void release(Context*) { }

  virtual void freeze(Context*) { }

  virtual void thaw(Context*) { }

  virtual OperandType type(Context*) = 0;

  virtual Assembler::Operand* asAssemblerOperand(Context*) = 0;

  Site* next;
};

class Stack: public StackElement {
 public:
  Stack(unsigned index, Value* value, Stack* next):
    index(index), value(value), next(next)
  { }

  unsigned index;
  Value* value;
  Stack* next;
};

class State {
 public:
  State(State* next, Stack* stack, Value** locals):
    stack(stack),
    locals(locals),
    next(next)
  { }

  Stack* stack;
  Value** locals;
  State* next;
};

class LogicalInstruction {
 public:
  Event* firstEvent;
  Event* lastEvent;
  LogicalInstruction* immediatePredecessor;
  Stack* stack;
  Value** locals;
  unsigned machineOffset;
  bool stackSaved;
};

class Register {
 public:
  Register(int number):
    value(0), site(0), number(number), size(0), refCount(0),
    freezeCount(0), reserved(false), pushed(false)
  { }

  Value* value;
  RegisterSite* site;
  int number;
  unsigned size;
  unsigned refCount;
  unsigned freezeCount;
  bool reserved;
  bool pushed;
};

class ConstantPoolNode {
 public:
  ConstantPoolNode(Promise* promise): promise(promise), next(0) { }

  Promise* promise;
  ConstantPoolNode* next;
};

class Junction {
 public:
  Junction(unsigned logicalIp, Junction* next):
    logicalIp(logicalIp),
    next(next)
  { }

  unsigned logicalIp;
  Junction* next;
};

class Read {
 public:
  Read():
    next(0), event(0), eventNext(0)
  { }
  
  Read* next;
  Event* event;
  Read* eventNext;

  virtual Site* pickSite(Context* c, Value* v) = 0;

  virtual Site* allocateSite(Context* c) = 0;

  virtual void intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex) = 0;
  
  virtual bool valid() = 0;
};

class Value: public Compiler::Operand {
 public:
  Value(Site* site, Site* target):
    reads(0), lastRead(0), sites(site), source(0), target(target)
  { }
  
  Read* reads;
  Read* lastRead;
  Site* sites;
  Site* source;
  Site* target;
};

enum Pass {
  ScanPass,
  CompilePass
};

class Context {
 public:
  Context(System* system, Assembler* assembler, Zone* zone,
          Compiler::Client* client):
    system(system),
    assembler(assembler),
    zone(zone),
    client(client),
    logicalIp(-1),
    state(new (zone->allocate(sizeof(State))) State(0, 0)),
    logicalCode(0),
    logicalCodeLength(0),
    parameterFootprint(0),
    localFootprint(0),
    registers
    (static_cast<Register**>
     (zone->allocate(sizeof(Register*) * assembler->registerCount()))),
    firstConstant(0),
    lastConstant(0),
    constantCount(0),
    nextSequence(0),
    junctions(0),
    machineCode(0),
    stackReset(false),
    constantCompare(CompareNone),
    pass(ScanPass),
    stackPadding(0)
  {
    for (unsigned i = 0; i < assembler->registerCount(); ++i) {
      registers[i] = new (zone->allocate(sizeof(Register))) Register(i);
    }

    registers[assembler->base()]->reserved = true;
    registers[assembler->stack()]->reserved = true;
    registers[assembler->thread()]->reserved = true;
  }

  System* system;
  Assembler* assembler;
  Zone* zone;
  Compiler::Client* client;
  int logicalIp;
  State* state;
  LogicalInstruction* logicalCode;
  unsigned logicalCodeLength;
  unsigned parameterFootprint;
  unsigned localFootprint;
  Register** registers;
  ConstantPoolNode* firstConstant;
  ConstantPoolNode* lastConstant;
  unsigned constantCount;
  unsigned nextSequence;
  Junction* junctions;
  uint8_t* machineCode;
  bool stackReset;
  ConstantCompare constantCompare;
  Pass pass;
  unsigned stackPadding;
};

class PoolPromise: public Promise {
 public:
  PoolPromise(Context* c, int key): c(c), key(key) { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>
        (c->machineCode + pad(c->assembler->length()) + (key * BytesPerWord));
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0;
  }

  Context* c;
  int key;
};

class CodePromise: public Promise {
 public:
  CodePromise(Context* c, CodePromise* next): c(c), offset(-1), next(next) { }

  CodePromise(Context* c, int offset): c(c), offset(offset), next(0) { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>(c->machineCode + offset);
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0 and offset >= 0;
  }

  Context* c;
  int offset;
  CodePromise* next;
};

class IpPromise: public Promise {
 public:
  IpPromise(Context* c, int logicalIp):
    c(c),
    logicalIp(logicalIp)
  { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>
        (c->machineCode + c->logicalCode[logicalIp].machineOffset);
    }

    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0;
  }

  Context* c;
  int logicalIp;
};

inline void NO_RETURN
abort(Context* c)
{
  abort(c->system);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->system, v);
}
#endif // not NDEBUG

inline void
expect(Context* c, bool v)
{
  expect(c->system, v);
}

class Event {
 public:
  Event(Context* c):
    next(0), stack(c->state->stack), locals(c->locals), promises(0), reads(0),
    readCount(0), sequence(c->nextSequence++), stackReset(c->stackReset)
  {
    assert(c, c->logicalIp >= 0);

    LogicalInstruction* i = c->logicalCode + c->logicalIp;
    if (i->lastEvent) {
      i->lastEvent->next = this;
    } else {
      i->firstEvent = this;
    }
    i->lastEvent = this;

    if (c->stackReset) {
//       fprintf(stderr, "stack reset\n");
      c->stackReset = false;
    }
  }

  Event(Context*, unsigned sequence, Stack* stack, Local* locals):
    next(0), stack(stack), locals(locals), promises(0), reads(0), readCount(0),
    sequence(sequence), stackReset(false)
  { }

  virtual ~Event() { }

  virtual void compile(Context* c) = 0;

  Event* next;
  Stack* stack;
  Value** locals;
  CodePromise* promises;
  Read* reads;
  unsigned readCount;
  unsigned sequence;
  bool stackReset;
};

int
localOffset(Context* c, int v)
{
  int parameterFootprint = c->parameterFootprint * BytesPerWord;

  v *= BytesPerWord;
  if (v < parameterFootprint) {
    return (parameterFootprint - v - BytesPerWord) + (BytesPerWord * 2);
  } else {
    return -(v + BytesPerWord - parameterFootprint);
  }
}

bool
findSite(Context*, Value* v, Site* site)
{
  for (Site* s = v->sites; s; s = s->next) {
    if (s == site) return true;
  }
  return false;
}

void
addSite(Context* c, Stack* stack, Value** locals, unsigned size, Value* v,
        Site* s)
{
  if (not findSite(c, v, s)) {
//     fprintf(stderr, "add site %p (%d) to %p\n", s, s->type(c), v);
    s->acquire(c, stack, locals, size, v);
    s->next = v->sites;
    v->sites = s;
  }
}

void
removeSite(Context* c, Value* v, Site* s)
{
  for (Site** p = &(v->sites); *p;) {
    if (s == *p) {
//       fprintf(stderr, "remove site %p (%d) from %p\n", s, s->type(c), v);
      s->release(c);
      *p = (*p)->next;
      break;
    } else {
      p = &((*p)->next);
    }
  }
}

void
removeMemorySites(Context* c, Value* v)
{
  for (Site** p = &(v->sites); *p;) {
    if ((*p)->type(c) == MemoryOperand) {
//       fprintf(stderr, "remove site %p (%d) from %p\n", *p, (*p)->type(c), v);
      (*p)->release(c);
      *p = (*p)->next;
      break;
    } else {
      p = &((*p)->next);
    }
  }
}

void
clearSites(Context* c, Value* v)
{
  for (Site* s = v->sites; s; s = s->next) {
    s->release(c);
  }
  v->sites = 0;
}

bool
valid(Read* r)
{
  return r and r->valid();
}

bool
live(Value* v)
{
  return valid(v->reads);
}

void
nextRead(Context* c, Value* v)
{
//   fprintf(stderr, "pop read %p from %p; next: %p\n", v->reads, v, v->reads->next);

  v->reads = v->reads->next;
  if (not live(v)) {
    clearSites(c, v);
  }
}

class ConstantSite: public Site {
 public:
  ConstantSite(Promise* value): value(value) { }

  virtual unsigned copyCost(Context*, Site* s) {
    return (s == this ? 0 : 1);
  }

  virtual OperandType type(Context*) {
    return ConstantOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context*) {
    return &value;
  }

  Assembler::Constant value;
};

ConstantSite*
constantSite(Context* c, Promise* value)
{
  return new (c->zone->allocate(sizeof(ConstantSite))) ConstantSite(value);
}

ResolvedPromise*
resolved(Context* c, int64_t value)
{
  return new (c->zone->allocate(sizeof(ResolvedPromise)))
    ResolvedPromise(value);
}

ConstantSite*
constantSite(Context* c, int64_t value)
{
  return constantSite(c, resolved(c, value));
}

class AddressSite: public Site {
 public:
  AddressSite(Promise* address): address(address) { }

  virtual unsigned copyCost(Context*, Site* s) {
    return (s == this ? 0 : 3);
  }

  virtual OperandType type(Context*) {
    return AddressOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context*) {
    return &address;
  }

  Assembler::Address address;
};

AddressSite*
addressSite(Context* c, Promise* address)
{
  return new (c->zone->allocate(sizeof(AddressSite))) AddressSite(address);
}

void
freeze(Register* r)
{
  if (DebugRegisters) {
    fprintf(stderr, "freeze %d to %d\n", r->number, r->freezeCount + 1);
  }

  ++ r->freezeCount;
}

void
thaw(Register* r)
{
  if (DebugRegisters) {
    fprintf(stderr, "thaw %d to %d\n", r->number, r->freezeCount - 1);
  }

  -- r->freezeCount;
}

Register*
acquire(Context* c, uint32_t mask, Stack* stack, Value** locals,
        unsigned newSize, Value* newValue, RegisterSite* newSite);

void
release(Context* c, Register* r);

Register*
validate(Context* c, uint32_t mask, Stack* stack, unsigned size,
         Value* value, RegisterSite* site, Register* current);

class RegisterSite: public Site {
 public:
  RegisterSite(uint64_t mask, Register* low = 0, Register* high = 0):
    mask(mask), low(low), high(high), register_(NoRegister, NoRegister)
  { }

  void sync(Context* c UNUSED) {
    assert(c, low);

    register_.low = low->number;
    register_.high = (high? high->number : NoRegister);
  }

  virtual unsigned copyCost(Context* c, Site* s) {
    sync(c);

    if (s and
        (this == s or
         (s->type(c) == RegisterOperand
          and (static_cast<RegisterSite*>(s)->mask
               & (static_cast<uint64_t>(1) << register_.low))
          and (register_.high == NoRegister
               or (static_cast<RegisterSite*>(s)->mask
                   & (static_cast<uint64_t>(1) << (register_.high + 32)))))))
    {
      return 0;
    } else {
      return 2;
    }
  }

  virtual void acquire(Context* c, Stack* stack, Value** locals, unsigned size,
                       Value* v)
  {
    low = ::validate(c, mask, stack, locals, size, v, this, low);
    if (size > BytesPerWord) {
      ::freeze(low);
      high = ::validate(c, mask >> 32, stack, locals, size, v, this, high);
      ::thaw(low);
    }
  }

  virtual void release(Context* c) {
    assert(c, low);

    ::release(c, low);
    if (high) {
      ::release(c, high);
    }
  }

  virtual void freeze(Context* c UNUSED) {
    assert(c, low);

    ::freeze(low);
    if (high) {
      ::freeze(high);
    }
  }

  virtual void thaw(Context* c UNUSED) {
    assert(c, low);

    ::thaw(low);
    if (high) {
      ::thaw(high);
    }
  }

  virtual OperandType type(Context*) {
    return RegisterOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context* c) {
    sync(c);
    return &register_;
  }

  uint64_t mask;
  Register* low;
  Register* high;
  Assembler::Register register_;
};

RegisterSite*
registerSite(Context* c, int low, int high = NoRegister)
{
  assert(c, low != NoRegister);
  assert(c, low < static_cast<int>(c->assembler->registerCount()));
  assert(c, high == NoRegister
         or high < static_cast<int>(c->assembler->registerCount()));

  Register* hr;
  if (high == NoRegister) {
    hr = 0;
  } else {
    hr = c->registers[high];
  }
  return new (c->zone->allocate(sizeof(RegisterSite)))
    RegisterSite(~static_cast<uint64_t>(0), c->registers[low], hr);
}

RegisterSite*
freeRegisterSite(Context* c, uint64_t mask = ~static_cast<uint64_t>(0))
{
  return new (c->zone->allocate(sizeof(RegisterSite)))
    RegisterSite(mask);
}

RegisterSite*
fixedRegisterSite(Context* c, int low, int high = NoRegister)
{
  uint64_t mask;
  if (high == NoRegister) {
    mask = (~static_cast<uint64_t>(0) << 32)
      | (static_cast<uint64_t>(1) << low);
  } else {
    mask = (static_cast<uint64_t>(1) << (high + 32))
      | (static_cast<uint64_t>(1) << low);
  }

  return new (c->zone->allocate(sizeof(RegisterSite)))
    RegisterSite(mask);
}

Register*
increment(Context* c, int i)
{
  Register* r = c->registers[i];

  if (DebugRegisters) {
    fprintf(stderr, "increment %d to %d\n", r->number, r->refCount + 1);
  }

  ++ r->refCount;

  return r;
}

void
decrement(Context* c UNUSED, Register* r)
{
  assert(c, r->refCount > 0);

  if (DebugRegisters) {
    fprintf(stderr, "decrement %d to %d\n", r->number, r->refCount - 1);
  }

  -- r->refCount;
}

class MemorySite: public Site {
 public:
  MemorySite(int base, int offset, int index, unsigned scale):
    base(0), index(0), offsetPromise(0), value(base, offset, index, scale)
  { }

  void sync(Context* c UNUSED) {
    assert(c, base);

    value.base = base->number;
    value.index = (index? index->number : NoRegister);
  }

  virtual unsigned copyCost(Context* c, Site* s) {
    sync(c);

    if (s and
        (this == s or
         (s->type(c) == MemoryOperand
          and static_cast<MemorySite*>(s)->value.base == value.base
          and static_cast<MemorySite*>(s)->value.offset == value.offset
          and static_cast<MemorySite*>(s)->value.index == value.index
          and static_cast<MemorySite*>(s)->value.scale == value.scale)))
    {
      return 0;
    } else {
      return 4;
    }
  }

  virtual void acquire(Context* c, Stack*, Value**, unsigned, Value*) {
    base = increment(c, value.base);
    if (value.index != NoRegister) {
      index = increment(c, value.index);
    }
  }

  virtual void release(Context* c) {
    decrement(c, base);
    if (index) {
      decrement(c, index);
    }
  }

  virtual OperandType type(Context*) {
    return MemoryOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context* c) {
    sync(c);
    return &value;
  }

  Register* base;
  Register* index;
  Assembler::Memory value;
};

MemorySite*
memorySite(Context* c, int base, int offset = 0, int index = NoRegister,
           unsigned scale = 1)
{
  return new (c->zone->allocate(sizeof(MemorySite)))
    MemorySite(base, offset, index, scale);
}

Site*
targetOrNull(Context* c, Value* v, Read* r)
{
  if (v->target) {
    return v->target;
  } else {
    Site* s = r->pickSite(c, v);
    if (s) return s;
    return r->allocateSite(c);
  }
}

Site*
targetOrNull(Context* c, Value* v)
{
  if (v->target) {
    return v->target;
  } else if (live(v)) {
    Site* s = r->pickSite(c, v);
    if (s) return s;
    return r->allocateSite(c);
  }
  return 0;
}

Site*
pickSite(Context* c, Value* value, uint8_t typeMask, uint64_t registerMask,
         int frameIndex)
{
  Site* site = 0;
  unsigned copyCost = 0xFFFFFFFF;
  for (Site* s = value->sites; s; s = s->next) {
    if (s->match(c, typeMask, registerMask, frameIndex)) {
      unsigned v = s->copyCost(c, 0);
      if (v < copyCost) {
        site = s;
        copyCost = v;
      }
    }
  }
  return site;
}

Site*
allocateSite(Context* c, uint8_t typeMask, uint64_t registerMask,
             int frameIndex)
{
  if ((typeMask & (1 << RegisterOperand)) and registerMask) {
    return freeRegisterSite(c, registerMask);
  } else if (frameIndex >= 0) {
    return frameSite(c, frameIndex);
  } else {
    abort(c);
  }
}

class TargetRead: public Read {
 public:
  TargetRead(Value* target, uint8_t typeMask, uint64_t registerMask,
             int frameIndex):
    target(target), registerMask(registerMask), typeMask(typeMask),
    frameIndex(frameIndex)
  { }

  virtual Site* pickSite(Context* c, Value* value) {
    return ::pickSite(c, value, typeMask, registerMask, frameIndex);
  }

  virtual Site* allocateSite(Context* c) {
    if (target) {
      Site* s = targetOrNull(c, target);
      if (s and s->match(c, typeMask, registerMask, frameIndex)) {
        return s;
      }
    }
    return ::allocateSite(c, typeMask, registerMask, frameIndex);
  }

  virtual void intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex)
  {
    *typeMask &= this->typeMask;
    *registerMask &= this->registerMask;

    if (*frameIndex == AnyFrameIndex) {
      *frameIndex = this->frameIndex;
    } else if (this->frameIndex != AnyFrameIndex
               and *frameIndex != this->frameIndex)
    {
      *frameIndex = NoFrameIndex;
    }
  }

  virtual bool valid() {
    return true;
  }

  Value* target;
  uint64_t registerMask;
  uint8_t typeMask;
  int frameIndex;
};

TargetRead*
targetRead(Context* c, Value* target = 0,
           uint8_t typeMask = ~static_cast<uint8_t>(0),
           uint64_t registerMask = ~static_cast<uint64_t>(0),
           int frameIndex = NoFrameIndex)
{
  return new (c->zone->allocate(sizeof(TargetRead)))
    TargetRead(target, typeMask, registerMask);
}

class MultiRead: public Read {
 public:
  MultiRead():
    reads(0)
  { }

  virtual Site* pickSite(Context* c) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::pickSite(c, value, typeMask, registerMask, frameIndex);
  }

  virtual Site* allocateSite(Context* c) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::allocateSite(c, typeMask, registerMask, frameIndex);
  }

  virtual void intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex)
  {
    for (Cell* cell = reads; cell; cell = cell->next) {
      Read* r = static_cast<Read*>(cell->value);
      r->intersect(&typeMask, &registerMask, &frameIndex);
    }
  }

  virtual bool valid() {
    for (Cell* cell = reads; cell; cell = cell->next) {
      Read* r = static_cast<Read*>(cell->value);
      if (r->valid()) {
        return true;
      }
    }
    return false;
  }

  Cell* reads;
};

MultiRead*
multiRead(Context* c)
{
  return new (c->zone->allocate(sizeof(MultiRead))) MultiRead;
}

VirtualSite*
anyRegisterSite(Context* c)
{
  return virtualSite(c, 0, 1 << RegisterOperand, ~static_cast<uint64_t>(0));
}

VirtualSite*
registerOrConstantSite(Context* c)
{
  return virtualSite(c, 0, (1 << RegisterOperand) | (1 << ConstantOperand),
                     ~static_cast<uint64_t>(0));
}

Site*
targetOrRegister(Context* c, Value* v)
{
  Site* s = targetOrNull(c, v);
  if (s) {
    return s;
  } else {
    return freeRegisterSite(c);
  }
}

Site*
pick(Context* c, Site* sites, Site* target = 0, unsigned* cost = 0)
{
  Site* site = 0;
  unsigned copyCost = 0xFFFFFFFF;
  for (Site* s = sites; s; s = s->next) {
    unsigned v = s->copyCost(c, target);
    if (v < copyCost) {
      site = s;
      copyCost = v;
    }
  }

  if (cost) *cost = copyCost;
  return site;
}

bool
trySteal(Context* c, Register* r, Stack* stack, Value** locals)
{
  assert(c, r->refCount == 0);

  Value* v = r->value;
  assert(c, v->reads);

  if (DebugRegisters) {
    fprintf(stderr, "try steal %d from %p: next: %p\n",
            r->number, v, v->sites->next);
  }

  if (v->sites->next == 0) {
    Site* saveSite = 0;
    for (unsigned i = 0; i < localFootprint; ++i) {
      if (locals[i] == v) {
        saveSite = frameSite(c, i);
        break;
      }
    }

    if (saveSite == 0) {
      for (Stack* s = stack; s and (not s->pushed); s = s->next) {
        if (s->value == v) {
          uint8_t typeMask;
          uint64_t registerMask;
          int frameIndex = AnyFrameIndex;
          v->reads->intersect(&typeMask, &registerMask, &frameIndex);

          if (frameIndex >= 0) {
            saveSite = frameSite(c, frameIndex);
          } else {
            saveSite = stackSite(c, s);
          }
          break;
        }
      }
    }

    if (saveSite) {
      apply(c, Move, r->size, r->site, saveSite);
      addSite(c, 0, 0, r->size, v, saveSite);
    } else {
      if (DebugRegisters) {
        fprintf(stderr, "unable to steal %d from %p\n", r->number, v);
      }
      return false;
    }
  }

  removeSite(c, v, r->site);

  return true;
}

bool
used(Context* c, Register* r)
{
  Value* v = r->value;
  return v and findSite(c, v, r->site);
}

bool
usedExclusively(Context* c, Register* r)
{
  return used(c, r) and r->value->sites->next == 0;
}

unsigned
registerCost(Context* c, Register* r)
{
  if (r->reserved or r->freezeCount) {
    return 6;
  }

  unsigned cost = 0;

  if (used(c, r)) {
    ++ cost;
    if (usedExclusively(c, r)) {
      cost += 2;
    }
  }

  if (r->refCount) {
    cost += 2;
  }

  return cost;
}

Register*
pickRegister(Context* c, uint32_t mask)
{
  Register* register_ = 0;
  unsigned cost = 5;
  for (int i = c->assembler->registerCount() - 1; i >= 0; --i) {
    if ((1 << i) & mask) {
      Register* r = c->registers[i];
      if ((static_cast<uint32_t>(1) << i) == mask) {
        return r;
      }

      unsigned myCost = registerCost(c, r);
      if (myCost < cost) {
        register_ = r;
        cost = myCost;
      }
    }
  }

  expect(c, register_);

  return register_;
}

void
swap(Context* c, Register* a, Register* b)
{
  assert(c, a != b);
  assert(c, a->number != b->number);

  Assembler::Register ar(a->number);
  Assembler::Register br(b->number);
  c->assembler->apply
    (Swap, BytesPerWord, RegisterOperand, &ar, RegisterOperand, &br);
  
  c->registers[a->number] = b;
  c->registers[b->number] = a;

  int t = a->number;
  a->number = b->number;
  b->number = t;
}

Register*
replace(Context* c, Stack* stack, Value** locals, Register* r)
{
  uint32_t mask = (r->freezeCount? r->site->mask : ~0);

  freeze(r);
  Register* s = acquire(c, mask, stack, locals, r->size, r->value, r->site);
  thaw(r);

  if (DebugRegisters) {
    fprintf(stderr, "replace %d with %d\n", r->number, s->number);
  }

  swap(c, r, s);

  return s;
}

Register*
acquire(Context* c, uint32_t mask, Stack* stack, Value** locals,
        unsigned newSize, Value* newValue, RegisterSite* newSite)
{
  Register* r = pickRegister(c, mask);

  if (r->reserved) return r;

  if (DebugRegisters) {
    fprintf(stderr, "acquire %d, value %p, site %p freeze count %d "
            "ref count %d used %d used exclusively %d\n",
            r->number, newValue, newSite, r->freezeCount, r->refCount,
            used(c, r), usedExclusively(c, r));
  }

  if (r->refCount) {
    r = replace(c, stack, r);
  } else {
    Value* oldValue = r->value;
    if (oldValue
        and oldValue != newValue
        and findSite(c, oldValue, r->site))
    {
      if (not trySteal(c, r, stack, locals)) {
        r = replace(c, stack, r);
      }
    }
  }

  r->size = newSize;
  r->value = newValue;
  r->site = newSite;

  return r;
}

void
release(Context*, Register* r)
{
  if (DebugRegisters) {
    fprintf(stderr, "release %d\n", r->number);
  }

  r->size = 0;
  r->value = 0;
  r->site = 0;  
}

Register*
validate(Context* c, uint32_t mask, Stack* stack, Value** locals,
         unsigned size, Value* value, RegisterSite* site, Register* current)
{
  if (current and (mask & (1 << current->number))) {
    if (current->reserved or current->value == value) {
      return current;
    }

    if (current->value == 0) {
      current->size = size;
      current->value = value;
      current->site = site;
      return current;
    } else {
      abort(c);
    }
  }

  Register* r = acquire(c, mask, stack, locals, size, value, site);

  if (current and current != r) {
    release(c, current);
    
    Assembler::Register rr(r->number);
    Assembler::Register cr(current->number);
    c->assembler->apply
      (Move, BytesPerWord, RegisterOperand, &cr, RegisterOperand, &rr);
  }

  return r;
}

void
apply(Context* c, UnaryOperation op, unsigned size, Site* a)
{
  OperandType type = a->type(c);
  Assembler::Operand* operand = a->asAssemblerOperand(c);

  c->assembler->apply(op, size, type, operand);
}

void
apply(Context* c, BinaryOperation op, unsigned size, Site* a, Site* b)
{
  OperandType aType = a->type(c);
  Assembler::Operand* aOperand = a->asAssemblerOperand(c);

  OperandType bType = b->type(c);
  Assembler::Operand* bOperand = b->asAssemblerOperand(c);

  c->assembler->apply(op, size, aType, aOperand, bType, bOperand);
}

void
insertRead(Context* c, Event* event, int sequence, Value* v, Read* r)
{
  event->reads = r;
  ++ event->readCount;

  //  fprintf(stderr, "add read %p to %p\n", r, v);

  if (sequence >= 0) {
    for (Read** p = &(v->reads); *p;) {
      if ((*p)->event->sequence > static_cast<unsigned>(sequence)) {
        r->next = *p;
        *p = r;
        break;
      } else {
        p = &((*p)->next);
      }
    }
  }

  if (r->next == 0) {
    if (v->lastRead) {
      v->lastRead->next = r;
    } else {
      v->reads = r;
    }
    v->lastRead = r;
  }
}

void
addRead(Context* c, Value* v, Read* r)
{
  insertRead(c, c->logicalCode[c->logicalIp].lastEvent, -1, v, r);
}

Site*
pushSite(Context*, PushEvent*);

class PushEvent: public Event {
 public:
  PushEvent(Context* c, Stack* s):
    Event(c), s(s), active(false)
  {
    assert(c, s->pushEvent == 0);

    s->pushEvent = this;
    addRead(c, s->value, s->geometry->size * BytesPerWord, pushSite(c, this));
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "PushEvent.compile active: %d\n", active);
    }

    if (active) {
      pushNow(c, s);
    }

    nextRead(c, s->value);
  }

  virtual bool skipMove(unsigned size) {
    return active and size >= BytesPerWord;
  }

  Stack* s;
  bool active;
};

void
appendPush(Context* c, Stack* s)
{
  if (DebugAppend) {
    fprintf(stderr, "appendPush\n");
  }

  new (c->zone->allocate(sizeof(PushEvent))) PushEvent(c, s);
}

void
appendPush(Context* c)
{
  appendPush(c, c->state->stack);
}

class PushedEvent: public Event {
 public:
  PushedEvent(Context* c, Stack* s):
    Event(c), s(s)
  { }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "PushedEvent.compile\n");
    }

    assert(c, c->stackPadding == 0);
    assert(c, s->geometry->padding == 0);

    s->pushSite = s->value->sites = pushSite(&c, s);
  }

  Stack* s;
};

void
appendPushed(Context* c, Stack* s)
{
  if (DebugAppend) {
    fprintf(stderr, "appendPushed\n");
  }

  new (c->zone->allocate(sizeof(PushedEvent))) PushedEvent(c, s);
}

void
push(Context* c, unsigned size, Value* v);

void
ignore(Context* c, unsigned count)
{
  if (count) {
    Assembler::Register stack(c->assembler->stack());
    Assembler::Constant offset(resolved(c, count * BytesPerWord));
    c->assembler->apply
      (Add, BytesPerWord, ConstantOperand, &offset, RegisterOperand, &stack);
  }
}

void
cleanStack(Context* c, Stack* stack, Local* locals, Read* reads)
{
  for (Local* l = locals; l; l = l->next) {
    l->reuse = false;
  }

  for (Stack* s = stack; s; s = s->next) {
    clearSites(c, s->value);
  }

  for (Stack* s = stack; s; s = s->next) {
    if (s->pushSite) {
      addSite(c, 0, 0, s->geometry->size * BytesPerWord, s->value,
              s->pushSite);
    }
  }

  for (Read* r = reads; r; r = r->eventNext) {
    nextRead(c, r->value);
  }
}

void
resetLocals(Context* c)
{
  for (Local* l = c->locals; l; l = l->next) {
    c->localTable[l->index] = 0;
  }
  c->locals = 0;
}

CodePromise*
codePromise(Context* c, Event* e)
{
  return e->promises = new (c->zone->allocate(sizeof(CodePromise)))
    CodePromise(c, e->promises);
}

CodePromise*
codePromise(Context* c, int offset)
{
  return new (c->zone->allocate(sizeof(CodePromise))) CodePromise(c, offset);
}

class CallEvent: public Event {
 public:
  CallEvent(Context* c, Value* address, unsigned flags,
            TraceHandler* traceHandler, Value* result, unsigned resultSize,
            Stack* argumentStack, unsigned argumentCount, unsigned padding):
    Event(c),
    address(address),
    traceHandler(traceHandler),
    result(result),
    flags(flags),
    resultSize(resultSize),
    argumentFootprint(0),
    paddding(padding)
  {
    uint32_t mask = ~0;
    Stack* s = argumentStack;
    unsigned index = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
      Site* target;
      if (index < c->assembler->argumentRegisterCount()) {
        int r = c->assembler->argumentRegister(index);
        target = fixedRegisterSite(c, r);
        mask &= ~(1 << r);
      } else {
        target = 0;
        s->pushEvent->active = true;
        argumentFootprint += s->geometry->size;
      }
      addRead(c, s->value, s->geometry->size * BytesPerWord, target);
      index += s->geometry->size;
      s = s->next;
    }

    addRead(c, address, BytesPerWord, virtualSite
            (c, 0, ~0, (static_cast<uint64_t>(mask) << 32) | mask));

    for (Stack* s = stack; s; s = s->next) {
      s->pushEvent->active = true;
      addRead(c, s->value, s->geometry->size * BytesPerWord, virtualSite
              (c, 0, ~0, (static_cast<uint64_t>(mask) << 32) | mask));
    }

    resetLocals(c);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "CallEvent.compile\n");
    }

    pushNow(c, stack);

    pad(c, padding);
    
    UnaryOperation type = ((flags & Compiler::Aligned) ? AlignedCall : Call);
    apply(c, type, BytesPerWord, address->source);

    if (traceHandler) {
      traceHandler->handleTrace(codePromise(c, c->assembler->length()));
    }

    cleanStack(c, stack, locals, reads);

    if (resultSize and live(result)) {
      addSite(c, 0, 0, resultSize, result, registerSite
              (c, c->assembler->returnLow(),
               resultSize > BytesPerWord ?
               c->assembler->returnHigh() : NoRegister));
    }

    if (argumentFootprint and ((flags & Compiler::NoReturn) == 0)) {
      ignore(c, argumentFootprint);
    }
  }

  Value* address;
  TraceHandler* traceHandler;
  Value* result;
  unsigned flags;
  unsigned resultSize;
  unsigned argumentFootprint;
  unsigned padding;
};

void
appendCall(Context* c, Value* address, unsigned flags,
           TraceHandler* traceHandler, Value* result, unsigned resultSize,
           Stack* argumentStack, unsigned argumentCount, unsigned padding)
{
  if (DebugAppend) {
    fprintf(stderr, "appendCall\n");
  }

  new (c->zone->allocate(sizeof(CallEvent)))
    CallEvent(c, address, flags, traceHandler, result,
              resultSize, argumentStack, argumentCount, unsigned padding);
}

class ReturnEvent: public Event {
 public:
  ReturnEvent(Context* c, unsigned size, Value* value):
    Event(c), value(value)
  {
    if (value) {
      addRead(c, value, size, fixedRegisterSite
              (c, c->assembler->returnLow(),
               size > BytesPerWord ?
               c->assembler->returnHigh() : NoRegister));
    }
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "ReturnEvent.compile\n");
    }

    if (value) {
      nextRead(c, value);
    }

    Assembler::Register base(c->assembler->base());
    Assembler::Register stack(c->assembler->stack());

    c->assembler->apply(Move, BytesPerWord, RegisterOperand, &base,
                        RegisterOperand, &stack);
    c->assembler->apply(Pop, BytesPerWord, RegisterOperand, &base);
    c->assembler->apply(Return);
  }

  Value* value;
};

void
appendReturn(Context* c, unsigned size, Value* value)
{
  if (DebugAppend) {
    fprintf(stderr, "appendReturn\n");
  }

  new (c->zone->allocate(sizeof(ReturnEvent))) ReturnEvent(c, size, value);
}

class MoveEvent: public Event {
 public:
  MoveEvent(Context* c, BinaryOperation type, unsigned size, Value* src,
            Value* dst, Site* srcTarget, VirtualSite* dstTarget):
    Event(c), type(type), size(size), src(src), dst(dst), dstTarget(dstTarget)
  {
    addRead(c, src, size, srcTarget);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "MoveEvent.compile\n");
    }

    bool isLoad = not valid(src->reads->next);
    bool isStore = not valid(dst->reads);

    Site* target = targetOrRegister(c, dst);
    unsigned cost = src->source->copyCost(c, target);
    if (cost == 0 and (isLoad or isStore)) {
      target = src->source;
    }

    assert(c, isLoad or isStore or target != src->source);

    if (target == src->source) {
      removeSite(c, src, target);
    }

    if (not isStore) {
      addSite(c, stack, locals, size, dst, target);
    }

    if (cost or type != Move) {
      if (match(c, target, dstTarget->typeMask, dstTarget->registerMask)) {
        apply(c, type, size, src->source, target);
      } else {
        assert(c, dstTarget->typeMask & (1 << RegisterOperand));

        Site* tmpTarget = freeRegisterSite(c, dstTarget->registerMask);

        addSite(c, stack, locals, size, dst, tmpTarget);

        apply(c, type, size, src->source, tmpTarget);

        if (isStore) {
          removeSite(c, dst, tmpTarget);

          apply(c, Move, size, tmpTarget, target);
        } else {
          removeSite(c, dst, target);          
        }
      }
    }

    if (isStore) {
      removeSite(c, dst, target);
    }

    nextRead(c, src);
  }

  BinaryOperation type;
  unsigned size;
  Value* src;
  Value* dst;
  VirtualSite* dstTarget;
};

void
appendMove(Context* c, BinaryOperation type, unsigned size, Value* src,
           Value* dst)
{
  if (DebugAppend) {
    fprintf(stderr, "appendMove\n");
  }

  VirtualSite* srcTarget = virtualSite(c, dst);
  VirtualSite* dstTarget = virtualSite(c);
  bool thunk;

  c->assembler->plan(type, size,
                     &(srcTarget->typeMask), &(srcTarget->registerMask),
                     &(dstTarget->typeMask), &(dstTarget->registerMask),
                     &thunk);

  assert(c, not thunk); // todo

  new (c->zone->allocate(sizeof(MoveEvent)))
    MoveEvent(c, type, size, src, dst, srcTarget, dstTarget);
}

ConstantSite*
findConstantSite(Context* c, Value* v)
{
  for (Site* s = v->sites; s; s = s->next) {
    if (s->type(c) == ConstantOperand) {
      return static_cast<ConstantSite*>(s);
    }
  }
  return 0;
}

class CompareEvent: public Event {
 public:
  CompareEvent(Context* c, unsigned size, Value* first, Value* second,
               Site* firstTarget, Site* secondTarget):
    Event(c), size(size), first(first), second(second)
  {
    addRead(c, first, size, firstTarget);
    addRead(c, second, size, secondTarget);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "CompareEvent.compile\n");
    }

    ConstantSite* firstConstant = findConstantSite(c, first);
    ConstantSite* secondConstant = findConstantSite(c, second);

    if (firstConstant and secondConstant) {
      int64_t d = firstConstant->value.value->value()
        - secondConstant->value.value->value();

      if (d < 0) {
        c->constantCompare = CompareLess;
      } else if (d > 0) {
        c->constantCompare = CompareGreater;
      } else {
        c->constantCompare = CompareEqual;
      }
    } else {
      c->constantCompare = CompareNone;

      apply(c, Compare, size, first->source, second->source);
    }

    nextRead(c, first);
    nextRead(c, second);
  }

  unsigned size;
  Value* first;
  Value* second;
};

void
appendCompare(Context* c, unsigned size, Value* first, Value* second)
{
  VirtualSite* firstTarget = virtualSite(c);
  VirtualSite* secondTarget = virtualSite(c);
  bool thunk;

  c->assembler->plan(Compare, size,
                     &(firstTarget->typeMask), &(firstTarget->registerMask),
                     &(secondTarget->typeMask), &(secondTarget->registerMask),
                     &thunk);

  assert(c, not thunk); // todo

  if (DebugAppend) {
    fprintf(stderr, "appendCompare\n");
  }

  new (c->zone->allocate(sizeof(CompareEvent)))
    CompareEvent(c, size, first, second, firstTarget, secondTarget);
}

void
preserve(Context* c, Stack* stack, unsigned size, Value* v, Site* s,
         Read* read)
{
  assert(c, v->sites == s);
  Site* r = targetOrNull(c, read);
  if (r == 0 or r == s) r = freeRegisterSite(c);
  addSite(c, stack, locals, size, v, r);
  apply(c, Move, size, s, r);
}

void
maybePreserve(Context* c, Stack* stack, unsigned size, Value* v, Site* s)
{
  if (valid(v->reads->next) and v->sites->next == 0) {
    preserve(c, stack, size, v, s, v->reads->next);
  }
}

class CombineEvent: public Event {
 public:
  CombineEvent(Context* c, BinaryOperation type, unsigned size, Value* first,
               Value* second, Value* result, Site* firstTarget,
               Site* secondTarget):
    Event(c), type(type), size(size), first(first), second(second),
    result(result)
  {
    // todo: we should really specify the sizes of each operand
    // seperately for binary operations.  The following is a hack
    // until then.
    unsigned firstSize;
    switch (type) {
    case ShiftLeft:
    case ShiftRight:
    case UnsignedShiftRight:
      firstSize = 4;
      break;

    default:
      firstSize = size;
      break;
    }

    addRead(c, first, firstSize, firstTarget);
    addRead(c, second, size, secondTarget);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "CombineEvent.compile\n");
    }

    maybePreserve(c, stack, size, second, second->source);

    apply(c, type, size, first->source, second->source);

    nextRead(c, first);
    nextRead(c, second);

    removeSite(c, second, second->source);
    if (live(result)) {
      addSite(c, 0, 0, size, result, second->source);
    }
  }

  BinaryOperation type;
  unsigned size;
  Value* first;
  Value* second;
  Value* result;
};

void
appendStackSync(Context* c);

Value*
value(Context* c, Site* site = 0, Site* target = 0)
{
  return new (c->zone->allocate(sizeof(Value))) Value(site, target);
}

void
appendCombine(Context* c, BinaryOperation type, unsigned size, Value* first,
              Value* second, Value* result)
{
  VirtualSite* firstTarget = virtualSite(c);
  VirtualSite* secondTarget = virtualSite(c, result);
  bool thunk;

  c->assembler->plan(type, size,
                     &(firstTarget->typeMask), &(firstTarget->registerMask),
                     &(secondTarget->typeMask), &(secondTarget->registerMask),
                     &thunk);

  if (thunk) {
    secondTarget->value = 0;

    Stack* oldStack = c->state->stack;

    ::push(c, size, second);
    ::push(c, size, first);

    Stack* argumentStack = c->state->stack;
    c->state->stack = oldStack;

    appendCall(c, value(c, constantSite(c, c->client->getThunk(type, size))),
               0, 0, result, size, argumentStack, 2);
  } else {
    if (DebugAppend) {
      fprintf(stderr, "appendCombine\n");
    }

    firstTarget->typeMask &= ~(1 << MemoryOperand);
    secondTarget->typeMask &= ~(1 << MemoryOperand);

    new (c->zone->allocate(sizeof(CombineEvent)))
      CombineEvent(c, type, size, first, second, result, firstTarget,
                   secondTarget);
  }
}

class TranslateEvent: public Event {
 public:
  TranslateEvent(Context* c, UnaryOperation type, unsigned size, Value* value,
                 Value* result, Site* target):
    Event(c), type(type), size(size), value(value), result(result)
  {
    addRead(c, value, size, target);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "TranslateEvent.compile\n");
    }

    maybePreserve(c, stack, size, value, value->source);

    apply(c, type, size, value->source);
    
    nextRead(c, value);

    removeSite(c, value, value->source);
    if (live(result)) {
      addSite(c, 0, 0, size, result, value->source);
    }
  }

  UnaryOperation type;
  unsigned size;
  Value* value;
  Value* result;
};

void
appendTranslate(Context* c, UnaryOperation type, unsigned size, Value* value,
                Value* result)
{
  if (DebugAppend) {
    fprintf(stderr, "appendTranslate\n");
  }

  VirtualSite* target = virtualSite(c, result);
  bool thunk;

  c->assembler->plan
    (type, size, &(target->typeMask), &(target->registerMask), &thunk);

  assert(c, not thunk); // todo

  target->typeMask &= ~(1 << MemoryOperand);

  new (c->zone->allocate(sizeof(TranslateEvent)))
    TranslateEvent(c, type, size, value, result, target);
}

class MemoryEvent: public Event {
 public:
  MemoryEvent(Context* c, Value* base, int displacement, Value* index,
              unsigned scale, Value* result):
    Event(c), base(base), displacement(displacement), index(index),
    scale(scale), result(result)
  {
    addRead(c, base, BytesPerWord, anyRegisterSite(c));
    if (index) addRead(c, index, BytesPerWord, registerOrConstantSite(c));
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "MemoryEvent.compile\n");
    }
    
    int indexRegister;
    int displacement = this->displacement;
    unsigned scale = this->scale;
    if (index) {
      ConstantSite* constant = findConstantSite(c, index);

      if (constant) {
        indexRegister = NoRegister;
        displacement += (constant->value.value->value() * scale);
        scale = 1;
      } else {
        assert(c, index->source->type(c) == RegisterOperand);
        indexRegister = static_cast<RegisterSite*>
          (index->source)->register_.low;
      }
    } else {
      indexRegister = NoRegister;
    }
    assert(c, base->source->type(c) == RegisterOperand);
    int baseRegister = static_cast<RegisterSite*>(base->source)->register_.low;

    nextRead(c, base);
    if (index) {
      if (BytesPerWord == 8 and indexRegister != NoRegister) {
        apply(c, Move4To8, 8, index->source, index->source);
      }

      nextRead(c, index);
    }

    result->target = memorySite
      (c, baseRegister, displacement, indexRegister, scale);
    addSite(c, 0, 0, 0, result, result->target);
  }

  Value* base;
  int displacement;
  Value* index;
  unsigned scale;
  Value* result;
};

void
appendMemory(Context* c, Value* base, int displacement, Value* index,
             unsigned scale, Value* result)
{
  if (DebugAppend) {
    fprintf(stderr, "appendMemory\n");
  }

  new (c->zone->allocate(sizeof(MemoryEvent)))
    MemoryEvent(c, base, displacement, index, scale, result);
}

Stack*
stack(Context* c, Value* value, StackGeometry* geometry, Stack* next)
{
  return new (c->zone->allocate(sizeof(Stack))) Stack(value, geometry, next);
}

Stack*
stack(Context* c, Value* value, unsigned size, unsigned index, Stack* next)
{
  return stack(c, value, new (c->zone->allocate(sizeof(StackGeometry)))
               StackGeometry(size, index), next);
}

void
resetStack(Context* c)
{
  unsigned i = 0;
  Stack* p = 0;
  for (Stack* s = c->state->stack; s; s = s->next) {
    Stack* n = stack(c, value(c), s->geometry, 0);
    n->value->sites = n->pushSite = pushSite(c, s);
    n->pushed = true;

    if (p) {
      p->next = n;
    } else {
      c->state->stack = n;
    }
    p = n;

    i += s->geometry->size;
  }

  resetLocals(c);

  c->stackReset = true;
}

class StackSyncEvent: public Event {
 public:
  StackSyncEvent(Context* c):
    Event(c)
  {
    for (Stack* s = stack; s; s = s->next) {
      if (s->pushEvent) s->pushEvent->active = true;
      addRead(c, s->value, s->geometry->size * BytesPerWord, 0);
    } 
  }

  StackSyncEvent(Context* c, unsigned sequence, Stack* stack, Local* locals):
    Event(c, sequence, stack, locals)
  {
    for (Stack* s = stack; s; s = s->next) {
      if (s->pushEvent) s->pushEvent->active = true;
      insertRead
        (c, this, sequence, s->value, s->geometry->size * BytesPerWord, 0);
    }
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "StackSyncEvent.compile\n");
    }

    cleanStack(c, stack, locals, reads);
  }
};

void
appendStackSync(Context* c)
{
  if (DebugAppend) {
    fprintf(stderr, "appendStackSync\n");
  }

  new (c->zone->allocate(sizeof(StackSyncEvent))) StackSyncEvent(c);
}

class BranchEvent: public Event {
 public:
  BranchEvent(Context* c, UnaryOperation type, Value* address):
    Event(c), type(type), address(address)
  {
    addRead(c, address, BytesPerWord, 0);
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "BranchEvent.compile\n");
    }

    bool jump;
    UnaryOperation type = this->type;
    if (type != Jump) {
      switch (c->constantCompare) {
      case CompareLess:
        switch (type) {
        case JumpIfLess:
        case JumpIfLessOrEqual:
        case JumpIfNotEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareGreater:
        switch (type) {
        case JumpIfGreater:
        case JumpIfGreaterOrEqual:
        case JumpIfNotEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareEqual:
        switch (type) {
        case JumpIfEqual:
        case JumpIfLessOrEqual:
        case JumpIfGreaterOrEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareNone:
        jump = true;
        break;

      default: abort(c);
      }
    } else {
      jump = true;
    }

    if (jump) {
      apply(c, type, BytesPerWord, address->source);
    }

    nextRead(c, address);
  }

  UnaryOperation type;
  Value* address;
};

void
appendBranch(Context* c, UnaryOperation type, Value* address)
{
  appendStackSync(c);

  if (DebugAppend) {
    fprintf(stderr, "appendBranch\n");
  }

  new (c->zone->allocate(sizeof(BranchEvent))) BranchEvent(c, type, address);

  resetStack(c);
}

class PushSite: public AbstractSite {
 public:
  PushSite(PushEvent* event): event(event) { }

  virtual Site* readTarget(Context* c, Read* r) {
    if (r->next and (not event->active)) {
      return targetOrNull(c, r->next);
    } else {
      return 0;
    }
  }

  PushEvent* event;
};

Site*
pushSite(Context* c, PushEvent* e)
{
  return new (c->zone->allocate(sizeof(PushSite))) PushSite(e);
}

class PopEvent: public Event {
 public:
  PopEvent(Context* c, unsigned count, bool ignore):
    Event(c), count(count), ignore(ignore)
  { }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "PopEvent.compile\n");
    }

    popNow(c, stack, count, ignore);
  }

  unsigned count;
  bool ignore;
};

void
appendPop(Context* c, unsigned count, bool ignore)
{
  if (DebugAppend) {
    fprintf(stderr, "appendPop\n");
  }

  new (c->zone->allocate(sizeof(PopEvent))) PopEvent(c, count, ignore);
}

class BoundsCheckEvent: public Event {
 public:
  BoundsCheckEvent(Context* c, Value* object, unsigned lengthOffset,
                   Value* index, intptr_t handler):
    Event(c), object(object), lengthOffset(lengthOffset), index(index),
    handler(handler)
  {
    addRead(c, object, BytesPerWord, anyRegisterSite(c));
    addRead(c, index, BytesPerWord, registerOrConstantSite(c));
  }

  virtual void compile(Context* c) {
    if (DebugCompile) {
      fprintf(stderr, "BoundsCheckEvent.compile\n");
    }

    Assembler* a = c->assembler;

    ConstantSite* constant = findConstantSite(c, index);
    CodePromise* nextPromise = codePromise(c, -1);
    CodePromise* outOfBoundsPromise = 0;

    if (constant) {
      expect(c, constant->value.value->value() >= 0);      
    } else {
      outOfBoundsPromise = codePromise(c, -1);

      apply(c, Compare, 4, constantSite(c, resolved(c, 0)), index->source);

      Assembler::Constant outOfBoundsConstant(outOfBoundsPromise);
      a->apply
        (JumpIfLess, BytesPerWord, ConstantOperand, &outOfBoundsConstant);
    }

    assert(c, object->source->type(c) == RegisterOperand);
    int base = static_cast<RegisterSite*>(object->source)->register_.low;

    Site* length = memorySite(c, base, lengthOffset);
    length->acquire(c, 0, 0, 0);

    apply(c, Compare, 4, index->source, length);

    length->release(c);

    Assembler::Constant nextConstant(nextPromise);
    a->apply(JumpIfGreater, BytesPerWord, ConstantOperand, &nextConstant);

    if (constant == 0) {
      outOfBoundsPromise->offset = a->length();
    }

    Assembler::Constant handlerConstant(resolved(c, handler));
    a->apply(Call, BytesPerWord, ConstantOperand, &handlerConstant);

    nextPromise->offset = a->length();

    nextRead(c, object);
    nextRead(c, index);
  }

  Value* object;
  unsigned lengthOffset;
  Value* index;
  intptr_t handler;
};

void
appendBoundsCheck(Context* c, Value* object, unsigned lengthOffset,
                  Value* index, intptr_t handler)
{
  if (DebugAppend) { 
    fprintf(stderr, "appendBoundsCheck\n");
  }

  new (c->zone->allocate(sizeof(BoundsCheckEvent))) BoundsCheckEvent
    (c, object, lengthOffset, index, handler);
}

Site*
readSource(Context* c, Stack* stack, Value** locals, Read* r)
{
  if (r->value->sites == 0) {
    return 0;
  }

  Site* target = (r->target ? r->target->readTarget(c, r) : 0);

  unsigned copyCost;
  Site* site = pick(c, r->value->sites, target, &copyCost);

  if (target and copyCost) {
    addSite(c, stack, locals, r->size, r->value, target);
    apply(c, Move, r->size, site, target);
    return target;
  } else {
    return site;
  }
}

Site*
pickJunctionSite(Context* c, Value* v)
{
  Site* s = r->pickSite(c, v);
  if (s) return s;
  return r->allocateSite(c);  
}

unsigned
resolveJunctionSite(Context* c, Event* e, Event* successor, Value* v,
                    unsigned index, Site** frozenSites,
                    unsigned frozenSiteIndex)
{
  if (live(v)) {
    Site* original = e->junctionSites[index];

    if (original == 0) {
      e->junctionSites[i] = pickJunctionSite(c, v);
    }

    Site* target = e->junctionSites[index];
    unsigned copyCost;
    Site* site = pick(c, v->sites, target, &copyCost);
    if (copyCost) {
      addSite(c, successor->stack, successor->locals, v, target);
      apply(c, Move, site, target);
    }

    if (original == 0) {
      frozenSites[frozenSiteIndex++] = target;
      target->freeze(c);
    }
  }

  return frozenSiteIndex;
}

void
propagateJunctionSites(Context* c, Event* e, Site** sites)
{
  for (Cell* pc = e->predecessors; pc; pc = pc->next) {
    Event* p = static_cast<Event*>(pc->value);
    if (p->junctionSites == 0) {
      p->junctionSites = sites;
      for (Cell* sc = p->successors; sc; sc = sc->next) {
        Event* s = static_cast<Event*>(sc->value);
        propagateJunctionSites(c, e, sites);
      }
    }
  }
}

void
compile(Context* c)
{
  Assembler* a = c->assembler;

  c->pass = CompilePass;

  Assembler::Register base(a->base());
  Assembler::Register stack(a->stack());
  a->apply(Push, BytesPerWord, RegisterOperand, &base);
  a->apply(Move, BytesPerWord, RegisterOperand, &stack,
           RegisterOperand, &base);

  if (stackOffset(c)) {
    Assembler::Constant offset(resolved(c, stackOffset(c) * BytesPerWord));
    a->apply(Subtract, BytesPerWord, ConstantOperand, &offset,
             RegisterOperand, &stack);
  }

  for (Event* e = c->firstEvent; e; e = e->next) {
    if (e->predecessors->next) {
      setSites(e, static_cast<Event*>(e->predecessors->value)->junctionSites);
    } else if (e->predecessors->successors->next) {
      setSites(e, static_cast<Event*>(e->predecessors->value)->savedSites);
    }

    { Site* frozenSites[e->readCount];
      unsigned frozenSiteIndex = 0;
      for (Read* r = e->reads; r; r = r->eventNext) {
        r->value->source = readSource(c, e->stack, e->locals, r);

        if (r->value->source) {
          assert(c, frozenSiteIndex < e->readCount);
          frozenSites[frozenSiteIndex++] = r->value->source;
          r->value->source->freeze(c);
        }
      }

      while (frozenSiteIndex) {
        frozenSites[--frozenSiteIndex]->thaw(c);
      }
    }

    e->compilePresync(c);

    unsigned frameCount
      = c->localCount + s->stack->index + footprint(s->stack);

    { Site* frozenSites[frameCount];
      unsigned frozenSiteIndex = 0;

      if (e->junctionSites) {
        for (unsigned i = 0; i < frameCount; ++i) {
          Site* site = e->junctionSites[i];
          if (site) {
            frozenSites[frozenSiteIndex++] = site;
            site->freeze(c);
          }
        }
      } else {
        for (Cell* sc = e->successors; sc; sc = sc->next) {
          Event* s = static_cast<Event*>(sc->value);
          if (s->predecessors->next) {
            unsigned size = sizeof(Site*) * frameCount;
            Site** junctionSites = static_cast<Site**>
              (c->zone->allocate(size));
            memset(junctionSites, 0, size);

            propagateJunctionSites(c, s, junctionSites);
            break;
          }
        }
      }

      if (e->junctionSites) {
        Event* s = e->next;
        for (unsigned i = 0; i < c->localCount; ++i) {
          frozenSiteIndex = resolveJunctionSite
            (c, e, s, s->locals[i], i, frozenSites, frozenSiteIndex);
        }

        unsigned i = s->stack->index + c->localCount;
        for (Stack* stack = s->stack; stack; stack = stack->next) {
          frozenSiteIndex = resolveJunctionSite
            (c, e, s, stack->value, i, frozenSites, frozenSiteIndex);

          i -= footprint(stack);
        }
      }

      while (frozenSiteIndex) {
        frozenSites[--frozenSiteIndex]->thaw(c);
      }
    }

    if (e->successors->next) {
      unsigned size = sizeof(Site*) * frameCount;
      Site** savedSites = static_cast<Site**>(c->zone->allocate(size));

      for (unsigned i = 0; i < c->localCount; ++i) {
        savedSites = s->locals[i]->sites;
      }

      unsigned i = s->stack->index + c->localCount;
      for (Stack* stack = s->stack; stack; stack = stack->next) {
        savedSites = stack->value->sites;

        i -= footprint(stack);
      }
    }

    e->compilePostsync(c);
        
    for (CodePromise* p = e->promises; p; p = p->next) {
      p->offset = a->offset();
    }
  }
}

unsigned
count(Stack* s)
{
  unsigned c = 0;
  while (s) {
    ++ c;
    s = s->next;
  }
  return c;
}

void
pushState(Context* c)
{
  if (DebugAppend) {
    unsigned count = 0; for (State* s = c->state; s; s = s->next) ++ count;
    fprintf(stderr, "push at level %d\n", count);
    count = 0; for (Stack* s = c->state->stack; s; s = s->next) ++ count;
    fprintf(stderr, "stack count: %d\n", count);
  }

  c->state = new (c->zone->allocate(sizeof(State)))
    State(c->state, c->state->stack);
}

void
saveStack(Context* c)
{
  if (c->logicalIp >= 0 and not c->logicalCode[c->logicalIp].stackSaved) {
    c->logicalCode[c->logicalIp].stackSaved = true;
    c->logicalCode[c->logicalIp].stack = c->state->stack;
    c->logicalCode[c->logicalIp].locals = c->locals;

    if (DebugAppend) {
      unsigned count = 0;
      for (Stack* s = c->state->stack; s; s = s->next) ++ count;
      fprintf(stderr, "stack count after ip %d: %d\n", c->logicalIp, count);
    }
  }
}

void
popState(Context* c)
{
  c->state = new (c->zone->allocate(sizeof(State)))
    State(c->state->next->next, c->state->next->stack);
 
  if (DebugAppend) {
    unsigned count = 0; for (State* s = c->state; s; s = s->next) ++ count;
    fprintf(stderr, "pop to level %d\n", count);
    count = 0; for (Stack* s = c->state->stack; s; s = s->next) ++ count;
    fprintf(stderr, "stack count: %d\n", count);
  }
}

Stack*
stack(Context* c, Value* value, unsigned size, Stack* next)
{
  return stack
    (c, value, size, (next ? next->geometry->index + next->geometry->size : 0),
     next);
}

void
push(Context* c, unsigned size, Value* v)
{
  assert(c, ceiling(size, BytesPerWord));

  c->state->stack = stack(c, v, ceiling(size, BytesPerWord), c->state->stack);

  appendPush(c);
}

Value*
pop(Context* c, unsigned size UNUSED)
{
  Stack* s = c->state->stack;
  assert(c, ceiling(size, BytesPerWord) == s->size);

  appendPop(c, s->geometry->size, false);

  c->state->stack = s->next;
  return s->value;
}

void
updateJunctions(Context* c)
{
  for (Junction* j = c->junctions; j; j = j->next) {
    LogicalInstruction* i = c->logicalCode + j->logicalIp;
    LogicalInstruction* p = i->immediatePredecessor;

    p->lastEvent = p->lastEvent->next
      = new (c->zone->allocate(sizeof(StackSyncEvent)))
      StackSyncEvent(c, p->lastEvent->sequence, p->stack, p->locals);
  }
}

void
visit(Context* c, unsigned logicalIp)
{
  assert(c, logicalIp < c->logicalCodeLength);

  if (c->logicalIp >= 0 and (not c->stackReset)) {
    assert(c, c->logicalCode[logicalIp].immediatePredecessor == 0);
    c->logicalCode[logicalIp].immediatePredecessor
      = c->logicalCode + c->logicalIp;
  }
}

class Client: public Assembler::Client {
 public:
  Client(Context* c): c(c) { }

  virtual int acquireTemporary(uint32_t mask) {
    int r = pickRegister(c, mask)->number;
    save(r);
    increment(c, r);
    return r;
  }

  virtual void releaseTemporary(int r) {
    decrement(c, c->registers[r]);
    restore(r);
  }

  virtual void save(int r) {
    if (c->registers[r]->refCount or c->registers[r]->value) {
      Assembler::Register operand(r);
      c->assembler->apply(Push, BytesPerWord, RegisterOperand, &operand);
      c->registers[r]->pushed = true;
    }
  }

  virtual void restore(int r) {
    if (c->registers[r]->pushed) {
      Assembler::Register operand(r);
      c->assembler->apply(Pop, BytesPerWord, RegisterOperand, &operand);
      c->registers[r]->pushed = false;
    }
  }

  Context* c;
};

class MyCompiler: public Compiler {
 public:
  MyCompiler(System* s, Assembler* assembler, Zone* zone,
             Compiler::Client* compilerClient):
    c(s, assembler, zone, compilerClient), client(&c)
  {
    assembler->setClient(&client);
  }

  virtual void pushState() {
    ::pushState(&c);
  }

  virtual void popState() {
    ::popState(&c);
  }

  virtual void saveStack() {
    ::saveStack(&c);
  }

  virtual void resetStack() {
    ::resetStack(&c);
  }

  virtual void init(unsigned logicalCodeLength, unsigned parameterFootprint,
                    unsigned localFootprint)
  {
    c.logicalCodeLength = logicalCodeLength;
    c.parameterFootprint = parameterFootprint;
    c.localFootprint = localFootprint;

    c.logicalCode = static_cast<LogicalInstruction*>
      (c.zone->allocate(sizeof(LogicalInstruction) * logicalCodeLength));
    memset(c.logicalCode, 0, sizeof(LogicalInstruction) * logicalCodeLength);

    c.localTable = static_cast<Local**>
      (c.zone->allocate(sizeof(Local*) * localFootprint));
    memset(c.localTable, 0, sizeof(Local*) * localFootprint);
  }

  virtual void visitLogicalIp(unsigned logicalIp) {
    visit(&c, logicalIp);

    c.stackReset = false;

    if (c.logicalCode[logicalIp].immediatePredecessor) {
      c.junctions = new (c.zone->allocate(sizeof(Junction)))
        Junction(logicalIp, c.junctions);
    }
  }

  virtual void startLogicalIp(unsigned logicalIp) {
    if (DebugAppend) {
      fprintf(stderr, " -- ip: %d\n", logicalIp);
    }

    visit(&c, logicalIp);

    ::saveStack(&c);

    c.logicalIp = logicalIp;
  }

  virtual Promise* machineIp(unsigned logicalIp) {
    return new (c.zone->allocate(sizeof(IpPromise))) IpPromise(&c, logicalIp);
  }

  virtual Promise* poolAppend(intptr_t value) {
    return poolAppendPromise(resolved(&c, value));
  }

  virtual Promise* poolAppendPromise(Promise* value) {
    Promise* p = new (c.zone->allocate(sizeof(PoolPromise)))
      PoolPromise(&c, c.constantCount);

    ConstantPoolNode* constant
      = new (c.zone->allocate(sizeof(ConstantPoolNode)))
      ConstantPoolNode(value);

    if (c.firstConstant) {
      c.lastConstant->next = constant;
    } else {
      c.firstConstant = constant;
    }
    c.lastConstant = constant;
    ++ c.constantCount;

    return p;
  }

  virtual Operand* constant(int64_t value) {
    return promiseConstant(resolved(&c, value));
  }

  virtual Operand* promiseConstant(Promise* value) {
    return ::value(&c, ::constantSite(&c, value));
  }

  virtual Operand* address(Promise* address) {
    return value(&c, ::addressSite(&c, address));
  }

  virtual Operand* memory(Operand* base,
                          int displacement = 0,
                          Operand* index = 0,
                          unsigned scale = 1)
  {
    Value* result = value(&c);

    appendMemory(&c, static_cast<Value*>(base), displacement,
                 static_cast<Value*>(index), scale, result);

    return result;
  }

  virtual Operand* stack() {
    Site* s = registerSite(&c, c.assembler->stack());
    return value(&c, s, s);
  }

  virtual Operand* base() {
    Site* s = registerSite(&c, c.assembler->base());
    return value(&c, s, s);
  }

  virtual Operand* thread() {
    Site* s = registerSite(&c, c.assembler->thread());
    return value(&c, s, s);
  }

  virtual Operand* label() {
    return value(&c, ::constantSite(&c, static_cast<Promise*>(0)));
  }

  Promise* machineIp() {
    return codePromise(&c, c.logicalCode[c.logicalIp].lastEvent);
  }

  virtual void mark(Operand* label) {
    appendStackSync(&c);
    ::resetStack(&c);

    for (Site* s = static_cast<Value*>(label)->sites; s; s = s->next) {
      if (s->type(&c) == ConstantOperand) {
        static_cast<ConstantSite*>(s)->value.value = machineIp();
        return;
      }
    }
    abort(&c);
  }

  virtual void push(unsigned size) {
    assert(&c, ceiling(size, BytesPerWord));

    c.state->stack = ::stack
      (&c, value(&c), ceiling(size, BytesPerWord), c.state->stack);
  }

  virtual void push(unsigned size, Operand* value) {
    ::push(&c, size, static_cast<Value*>(value));
  }

  virtual Operand* pop(unsigned size) {
    return ::pop(&c, size);
  }

  virtual void pushed() {
    Value* v = value(&c);
    c.state->stack = ::stack(&c, v, 1, c.state->stack);
    c.state->stack->pushed = true;
    appendPushed(&c, c.state->stack);
  }

  virtual void popped() {
    appendPop(&c, c.state->stack->size, true);

    c.state->stack = c.state->stack->next;
  }

  virtual StackElement* top() {
    return c.state->stack;
  }

  virtual unsigned size(StackElement* e) {
    return static_cast<Stack*>(e)->geometry->size;
  }

  virtual unsigned padding(StackElement* e) {
    return static_cast<Stack*>(e)->geometry->padding;
  }

  virtual Operand* peek(unsigned size UNUSED, unsigned index) {
    Stack* s = c.state->stack;
    for (unsigned i = index; i > 0;) {
      i -= s->geometry->size;
      s = s->next;
    }
    assert(&c, s->geometry->size == ceiling(size, BytesPerWord));
    return s->value;
  }

  virtual Operand* call(Operand* address,
                        unsigned flags,
                        TraceHandler* traceHandler,
                        unsigned resultSize,
                        unsigned argumentCount,
                        ...)
  {
    va_list a; va_start(a, argumentCount);

    unsigned footprint = 0;
    unsigned size = BytesPerWord;
    Value* arguments[argumentCount];
    unsigned argumentSizes[argumentCount];
    unsigned index = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
      Value* o = va_arg(a, Value*);
      if (o) {
        arguments[index] = o;
        argumentSizes[index] = size;
        size = BytesPerWord;
        ++ index;
      } else {
        size = 8;
      }
      ++ footprint;
    }

    va_end(a);

    for (Stack* s = c.state->stack; s; s = s->next) {
      if (s->pushEvent == 0) {
        appendPush(&c, s);
      }
      s->pushEvent->active = true;
    }

    Stack* oldStack = c.state->stack;
    Stack* bottomArgument = 0;

    for (int i = index - 1; i >= 0; --i) {
      ::push(&c, argumentSizes[i], arguments[i]);
      if (i == index - 1) {
        bottomArgument = c.state->stack;
      }
    }
    Stack* argumentStack = c.state->stack;
    c.state->stack = oldStack;

    unsigned padding = c->assembler->stackPadding
      (c.state->stack->geometry->index + c.state->stack->geometry->size);

    if (bottomArgument) {
      bottomArgument->geometry->padding = padding;
    }

    Value* result = value(&c);
    appendCall(&c, static_cast<Value*>(address), flags, traceHandler, result,
               resultSize, argumentStack, index, argumentCount ? 0 : padding);

    return result;
  }

  virtual Operand* stackCall(Operand* address,
                             unsigned flags,
                             TraceHandler* traceHandler,
                             unsigned resultSize,
                             unsigned argumentFootprint)
  {
    unsigned padding = c->assembler->stackPadding
      (c.state->stack->geometry->index + c.state->stack->geometry->size);

    int footprint = argumentFootprint;
    for (Stack* s = c.state->stack; s; s = s->next) {
      footprint -= s->geometry->size;
      if (footprint == 0) {
        s->geometry->padding = padding;
      }

      if (s->pushEvent == 0) {
        appendPush(&c, s);
      }
      s->pushEvent->active = true;
    }

    Value* result = value(&c);
    appendCall(&c, static_cast<Value*>(address), flags, traceHandler, result,
               resultSize, c.state->stack, 0, argumentFootprint ? 0 : padding);

    return result;
  }

  virtual void return_(unsigned size, Operand* value) {
    appendReturn(&c, size, static_cast<Value*>(value));
  }

  virtual void storeLocal(unsigned size, Operand* src, unsigned index) {
    assert(&c, index < c.localFootprint);

    if (c.state->locals[index]) {
      appendClobberLocal(&c, size, c.state->locals[index]);
    }

    Value* v = static_cast<Value*>(memory(base(), localOffset(&c, index)));
    store(size, src, v);

    c.state->locals[index] = v;
  }

  virtual Operand* loadLocal(unsigned size, unsigned index) {
    assert(&c, index < c.localFootprint);
    assert(&c, c.state->locals[index]);

    return c.state->locals[index];
  }

  virtual void checkBounds(Operand* object, unsigned lengthOffset,
                           Operand* index, intptr_t handler)
  {
    appendBoundsCheck(&c, static_cast<Value*>(object),
                      lengthOffset, static_cast<Value*>(index), handler);
  }

  virtual void store(unsigned size, Operand* src, Operand* dst) {
    appendMove(&c, Move, size, static_cast<Value*>(src),
               static_cast<Value*>(dst));
  }

  virtual Operand* load(unsigned size, Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, Move, size, static_cast<Value*>(src), dst);
    return dst;
  }

  virtual Operand* loadz(unsigned size, Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, MoveZ, size, static_cast<Value*>(src), dst);
    return dst;
  }

  virtual Operand* load4To8(Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, Move4To8, 8, static_cast<Value*>(src), dst);
    return dst;
  }

  virtual Operand* lcmp(Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, LongCompare, 8, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual void cmp(unsigned size, Operand* a, Operand* b) {
    appendCompare(&c, size, static_cast<Value*>(a),
                  static_cast<Value*>(b));
  }

  virtual void jl(Operand* address) {
    appendBranch(&c, JumpIfLess, static_cast<Value*>(address));
  }

  virtual void jg(Operand* address) {
    appendBranch(&c, JumpIfGreater, static_cast<Value*>(address));
  }

  virtual void jle(Operand* address) {
    appendBranch(&c, JumpIfLessOrEqual, static_cast<Value*>(address));
  }

  virtual void jge(Operand* address) {
    appendBranch(&c, JumpIfGreaterOrEqual, static_cast<Value*>(address));
  }

  virtual void je(Operand* address) {
    appendBranch(&c, JumpIfEqual, static_cast<Value*>(address));
  }

  virtual void jne(Operand* address) {
    appendBranch(&c, JumpIfNotEqual, static_cast<Value*>(address));
  }

  virtual void jmp(Operand* address) {
    appendBranch(&c, Jump, static_cast<Value*>(address));
  }

  virtual Operand* add(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Add, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* sub(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Subtract, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* mul(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Multiply, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* div(unsigned size, Operand* a, Operand* b)  {
    Value* result = value(&c);
    appendCombine(&c, Divide, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* rem(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Remainder, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* shl(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, ShiftLeft, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* shr(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, ShiftRight, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* ushr(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, UnsignedShiftRight, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* and_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, And, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* or_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Or, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* xor_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Xor, size, static_cast<Value*>(a),
                  static_cast<Value*>(b), result);
    return result;
  }

  virtual Operand* neg(unsigned size, Operand* a) {
    Value* result = value(&c);
    appendTranslate(&c, Negate, size, static_cast<Value*>(a), result);
    return result;
  }

  virtual unsigned compile() {
    updateJunctions(&c);
    ::compile(&c);
    return c.assembler->length();
  }

  virtual unsigned poolSize() {
    return c.constantCount * BytesPerWord;
  }

  virtual void writeTo(uint8_t* dst) {
    c.machineCode = dst;
    c.assembler->writeTo(dst);

    int i = 0;
    for (ConstantPoolNode* n = c.firstConstant; n; n = n->next) {
      *reinterpret_cast<intptr_t*>(dst + pad(c.assembler->length()) + i)
        = n->promise->value();
      i += BytesPerWord;
    }
  }

  virtual void dispose() {
    // ignore
  }

  Context c;
  ::Client client;
};

} // namespace

namespace vm {

Compiler*
makeCompiler(System* system, Assembler* assembler, Zone* zone,
             Compiler::Client* client)
{
  return new (zone->allocate(sizeof(MyCompiler)))
    MyCompiler(system, assembler, zone, client);
}

} // namespace vm
