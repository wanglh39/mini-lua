local Animal = {}
Animal.speak = function(self) return "some sound" end
local dog = {}
setmetatable(dog, {__index = Animal})
dog.speak = function(self) return "Woof!" end
local cat = {}
setmetatable(cat, {__index = Animal})
print(dog.speak(dog))
print(cat.speak(cat))

local log = {}
local readonly = {}
setmetatable(readonly, {__newindex = function(t, k, v)
    log[#log + 1] = k .. "=" .. v
end})
readonly.x = 10
readonly.y = 20
print(log[1])
print(log[2])
print(readonly.x)

local Base = {}
Base.method = function(self) return "base method" end
local Derived = {}
setmetatable(Derived, {__index = Base})
local obj = {}
setmetatable(obj, {__index = Derived})
print(obj.method(obj))