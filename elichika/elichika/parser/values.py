import chainer
import chainer.functions as F
import chainer.links as L
import inspect
import ast, gast
import weakref
from elichika.parser import vevaluator
from elichika.parser import core
from elichika.parser import nodes
from elichika.parser import values
from elichika.parser import functions
from elichika.parser import utils

from elichika.parser.functions import FunctionBase, UserDefinedFunction

fields = []
attributes = []

def reset_field_and_attributes():
    global fields
    global attributes
    fields = []
    attributes = []

def register_field(field : 'Field'):
    fields.append(weakref.ref(field))

def register_attribute(attribute : 'Attribute'):
    attributes.append(weakref.ref(attribute))
    
def commit(commit_id : 'str'):
    for field in fields:
        o = field()
        if o is not None:
            o.commit(commit_id)

    for attribute in attributes:
        o = attribute()
        if o is not None:
            o.commit(commit_id)

def checkout(commit_id : 'str'):
    for field in fields:
        o = field()
        if o is not None:
            o.checkout(commit_id)

    for attribute in attributes:
        o = attribute()
        if o is not None:
            o.checkout(commit_id)

class Field():
    def __init__(self, module : 'Field', parent : 'Field'):
        self.attributes = {}
        self.attributes_from_parent = []
        self.module = module
        self.parent = parent

        self.rev_attributes = {}
        self.rev_attributes_from_parent = {}
        
        register_field(self)

    def get_field(self) -> 'Field':
        return self

    def has_attribute(self, key) -> 'Boolean':
        return key in self.attributes.keys()

    def get_attribute(self, key : 'str') -> 'Attribute':
        if key in self.attributes.keys():
            return self.attributes[key]
        else:
            # search an attribute from parents
            attribute = None
            if self.parent is not None:
                attribute = self.parent.__get_attribute_from_child(key)
                if attribute is not None and not attribute in self.attributes_from_parent:
                    self.attributes_from_parent.append(attribute)

            if attribute is not None:
                return attribute

            # search an attribute from a module
            if self.module is not None:
                attribute = self.module.__get_attribute_from_child(key)

            if attribute is not None:
                return attribute

            attribute = Attribute(key)
            self.attributes[key] = attribute
            return attribute

    def commit(self, commit_id : 'str'):
        self.rev_attributes[commit_id] = self.attributes.copy()
        self.rev_attributes_from_parent[commit_id] = self.attributes_from_parent.copy()

    def checkout(self, commit_id : 'str'):
        if commit_id in self.rev_attributes:
            self.attributes = self.rev_attributes[commit_id].copy()
            self.attributes_from_parent = self.rev_attributes_from_parent[commit_id].copy()
        else:
            self.attributes = {}
            self.attributes_from_parent = []

    def __get_attribute_from_child(self, key : 'str') -> 'Attribute':
        if key in self.attributes.keys():
            return self.attributes[key]
        else:
            if self.parent is not None:
                return self.parent.__get_attribute_from_child(key)
            return None

class AttributeHistory:
    def __init__(self, value : 'Value'):
        self.value = value

class Attribute:
    def __init__(self, name : str):
        self.name : str = name
        self.history = []
        self.rev_history = {}
        self.access_num = 0
        self.rev_access_num = {}
        register_field(self)

    def revise(self, value : 'Value'):
        # assgin name to the value
        if value.name == "":
            value.name = self.name
            
        hist = AttributeHistory(value)
        self.history.append(hist)

    def has_value(self):
        return len(self.history) > 0

    def get_value(self):
        assert len(self.history) > 0
        self.access_num += 1
        return self.history[-1].value

    def commit(self, commit_id : 'str'):
        self.rev_history[commit_id] = self.history.copy()
        self.rev_access_num[commit_id] = self.access_num

    def checkout(self, commit_id : 'str'):
        if commit_id in self.rev_history:
            self.history = self.rev_history[commit_id].copy()
            self.access_num = self.rev_access_num[commit_id]
        else:
            self.history = []
            self.access_num = 0

    def has_diff(self, commit_id1 : 'str', commit_id2 : 'str'):
        if len(self.rev_history[commit_id1]) != len(self.rev_history[commit_id2]):
            return True
        for i in range(len(self.rev_history[commit_id1])):
            if self.rev_history[commit_id1][i] != self.rev_history[commit_id2][i]:
                return True

        return False

    def has_accessed(self, commit_id1 : 'str', commit_id2 : 'str'):
        return self.rev_access_num[commit_id1] != self.rev_access_num[commit_id2]

    def __str__(self):
        return self.name

class Value():
    def __init__(self):
        self.name : str = ""
        self.generator : nodes.Node = None
        self.onnx_name : str = ""

    def get_value(self) -> 'Value':
        return self

    def get_field(self) -> 'Field':
        return None

    def has_value(self) -> 'bool':
        return True

    def try_get_func(self, name : 'str') -> 'FuncValue':
        return None

    def __str__(self):
        return self.name

class NoneValue(Value):
    def __init__(self):
        super().__init__()

    def __str__(self):
        return self.name + '({})'.format('None')

class NumberValue(Value):
    def __init__(self, number):
        super().__init__()
        self.number = number

    def __str__(self):
        if self.number == None:
            return self.name + '(N.{})'.format('Any')
        return self.name + '(N.{})'.format(self.number)

class StrValue(Value):
    def __init__(self, string):
        super().__init__()
        self.string = string

    def __str__(self):
        if self.string == None:
            return self.name + '(S.{})'.format('Any')
        return self.name + '(S.{})'.format(self.string)

class BoolValue(Value):
    def __init__(self, b):
        super().__init__()
        self.b = b

    def __str__(self):
        if self.b == None:
            return self.name + '(B.{})'.format('Any')
        return self.name + '(B.{})'.format(self.b)

class RangeValue(Value):
    def __init__(self):
        super().__init__()
    def __str__(self):
        return self.name + '(R)'

class TupleValue(Value):
    def __init__(self, values = []):
        super().__init__()
        self.values = values
    def __str__(self):
        return self.name + '({})'.format(",".join([str(x) for x in self.values]))

class FuncValue(Value):
    def __init__(self, func : 'functions.FunctionBase', value : 'Value'):
        super().__init__()
        self.func = func
        self.value = value
    def __str__(self):
        return self.name + '(F)'

class ListValue(Value):
    def __init__(self):
        super().__init__()
        self.attributes = Field(None, None)

    def get_field(self) -> 'Field':
        return self.attributes

class DictValue(Value):
    def __init__(self):
        super().__init__()
        self.attributes = Field(None, None)

    def get_field(self) -> 'Field':
        return self.attributes

class TensorValue(Value):
    def __init__(self):
        super().__init__()
        self.shape = ()
        self.value = None
    def __str__(self):
        return self.name + '(T.{})'.format(self.shape)

class Type(Value):
    def __init__(self, name : 'str'):
        super().__init__()
        self.name = name

class Instance(Value):
    def __init__(self, module : 'Field', inst):
        super().__init__()
        self.attributes = Field(module, None)
        self.inst = inst
        self.callable = False
        self.func = None

    def get_field(self) -> 'Field':
        return self.attributes

class UserDefinedInstance(Instance):
    def __init__(self, module : 'Field', inst):
        super().__init__(module, inst)

    def try_get_func(self, name : 'str') -> 'FuncValue':
        
        attribute = self.attributes.get_attribute(name)
        if attribute.has_value():
            return attribute.get_value()

        if not hasattr(self.inst, name):
            return None

        attr_func = getattr(self.inst, name)
        if attr_func is None:
            return None

        func = UserDefinedFunction(attr_func)
        func_value = FuncValue(func, self)
        attribute.revise(func_value)

        return func_value
        