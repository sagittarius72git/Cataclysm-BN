--[[
  Craft Missing Parts - main.lua

  作りたい品（または建設したいもの）を選ぶと、足りない部品を自動で作ってそろえる。

  流れ
    1. アクションメニュー →「足りない部品を作る」で目標を選ぶ
    2. 足りないものと、作れるかどうかを確認画面に出す
    3. 実行すると、1回の製作ごとに「今すぐ作れる部品」を探して作る
         部品が作れなければ、その部品の材料（部品の部品）へさかのぼって探す
    4. 部品がそろったら
         製作 … 目標の品も作る
         建設 … 「材料がそろいました」と知らせて終わる（建てるのは建設メニューから）

  作り方の決め方
    毎回、手持ちと周り6マス（本体の製作と同じ範囲）を見直してから次の1回を決める。
    先に全部の手順を決めないので、途中で材料を拾ったり使ったりしても破綻しない。

  製作そのものは本体の製作（Character:make_craft）を使うので、
  技能・失敗・材料の選択画面・かかる時間はメニューから作るときと同じ。

  必要な本体の機能（PR #10477 で追加）
    Recipe:get_requirements / Character:can_make / Character:make_craft
    ConstructionRaw / Character:can_build（建設を選ぶときだけ）
  これらがない本体では、メニューを開いたときに知らせるだけで何もしない。

  [English]
  Pick an item to craft (or something to build) and this mod crafts the
  parts you are missing, going back through parts of parts as needed.
    1. Action menu -> "Craft missing parts", then choose a target
    2. A confirmation screen lists what is missing and whether it can be made
    3. After each craft, look for one part that can be made right now;
       if none, search the recipes of that part's own components
    4. When everything is ready: craft the target (recipe), or tell the
       player the materials are ready (construction; build it from the menu)
  Each step re-checks what you carry and items within 6 tiles, so picking up
  or using items mid-way does not break the plan.
  Crafting itself goes through Character:make_craft, so skills, failure,
  component prompts and time are the same as crafting from the menu.
  Needs the Lua functions added in PR #10477; without them the mod only
  shows a notice when the menu is opened.

  Author: sagittarius72
  Assisted-by: Claude:claude-opus-5-5
]]

gdebug.log_info("CMP: main.")

local mod = game.mod_runtime[game.current_mod]
local T = locale.gettext

----------------------------------------------------------------------
-- 調整値
-- Settings
----------------------------------------------------------------------

local CFG = {
  -- 部品の部品…とさかのぼる最大の段数
  -- How many levels of parts-of-parts to search
  max_depth = 6,
  -- 1回の実行で作る部品の最大回数（予想外のくり返しを止めるため）
  -- Max parts crafted in one run (stops unexpected loops)
  max_crafts = 50,
  -- 同じ部品のレシピを作る最大の回数
  -- Max times the same part recipe is crafted
  max_same = 20,
  -- 作り方を探すときに調べるレシピの最大数（重くなりすぎないように）
  -- Max recipes checked per search (keeps it fast)
  search_budget = 3000,
  -- 確認画面で「作れるか」を調べるときの上限（足りないもの1つごと）
  -- Search limit for the "can make" check on the confirm screen (per missing group)
  preview_budget = 500,
  -- 一覧に出す最大件数
  -- Max entries shown in a list
  max_list = 300,
  -- 確認画面に出す「足りないもの」の最大行数
  -- Max "missing" lines on the confirm screen
  max_preview = 12,
}

local ACT_CRAFT = "ACT_CRAFT"

-- 道具の品質のID → 名前（本体の翻訳がそのまま使われる）
-- Tool quality id -> name (the game's own translation is used)
local QUALITY_NAMES = {
  CUT = "cutting", CUT_FINE = "fine cutting", GLARE = "glare protection", SHEAR = "shearing",
  CHURN = "churn", LEATHER_AWL = "awl", SEW_CURVED = "curved needle", ANESTHESIA = "anesthesia",
  FISHING = "fishing", SMOOTH = "smoothing", WELD = "welding", HAMMER = "hammering",
  HAMMER_FINE = "fine hammering", SAW_W = "wood sawing", SAW_M = "metal sawing",
  SAW_M_FINE = "fine metal sawing", COOK = "food cooking", BOIL = "boiling", CONTAIN = "containing",
  CHEM = "chemical making", SMOKE_PIPE = "smoking", DISTILL = "distilling", AXE = "tree cutting",
  DIG = "digging", WRENCH = "bolt turning", WRENCH_FINE = "fine bolt turning", SCREW = "screw driving",
  SCREW_FINE = "fine screw driving", BUTCHER = "butchering", DRILL = "drilling", PRY = "prying",
  LIFT = "lifting", JACK = "jacking", SELF_JACK = "self jacking", HOSE = "siphoning",
  CHISEL = "chiseling", SEW = "sewing", KNIT = "knitting", PULL = "bullet pulling", ANVIL = "anvil",
  ANALYSIS = "analysis", CONCENTRATE = "concentration", SEPARATE = "separation",
  FINE_DISTILL = "fine distillation", CHROMATOGRAPHY = "chromatography", GRIND = "grinding",
  REAM = "reaming", FILE = "filing", VISE = "clamping", PRESSURIZATION = "pressurizing",
  LOCKPICK = "lockpicking", EXTRACT = "extraction", FILTER = "filtration", SANDING = "sanding",
  HV_INSULATION = "high voltage insulation", VOLT_MEASURE = "voltage measurement",
  PULL_FINE = "fine pulling", CUT_M = "metal cutting", GRIP_HOT = "hot gripping",
  CONTAIN_HOT = "hot containing", SWAGING = "swaging", GLASSBLOWING = "glassblowing",
  CASING_FORMING = "casing forming", CASING_PRESSING = "ammunition reloading", SLEEP_AID = "sleep aid",
}

----------------------------------------------------------------------
-- 補助
-- Helpers
----------------------------------------------------------------------

local function say(text)
  gapi.add_msg(text)
end

-- エラーで止まらないように包む（失敗したら fallback を返す）
-- Run f without stopping on errors (returns fallback on failure)
local function try(f, fallback)
  local ok, v = pcall(f)
  if ok then return v end
  return fallback
end

-- 本体に PR #10477 の機能があるか
-- Whether the game has the functions from PR #10477
local function has_recipe_api()
  return try(function()
    local you = gapi.get_avatar()
    return RecipeRaw.get_requirements ~= nil
      and you.can_make ~= nil
      and you.make_craft ~= nil
  end, false) == true
end

local function has_construction_api()
  return try(function()
    return ConstructionRaw ~= nil
      and ConstructionRaw.get_all ~= nil
      and gapi.get_avatar().can_build ~= nil
  end, false) == true
end

-- アイテムの名前（n個のときの表記）
-- Item name (form used for n items)
local function item_name(type_str, n)
  return try(function() return ItypeId.new(type_str):obj():get_name(n or 1) end, type_str)
end

-- 数えるときに「個数」ではなく「量（チャージ）」で数えるアイテムか（糸・接着剤など）
-- Whether the item is counted by charges instead of count (thread, glue, ...)
local function counted_by_charges(type_str)
  return try(function() return ItypeId.new(type_str):obj():is_stackable() end, false) == true
end

local function quality_name(q_str)
  return T(QUALITY_NAMES[q_str] or q_str)
end

-- 今している活動のID（何もしていなければ ""）
-- Id of the current activity ("" when idle)
local function activity_of(you)
  local s = try(function()
    local a = you:get_activity()
    if not a then return "" end
    return a:id_str()
  end, "")
  if s == nil or s == "ACT_NULL" or s == "null" then return "" end
  return s
end

----------------------------------------------------------------------
-- レシピの索引（完成品 → レシピ）
-- Recipe index (result item -> recipes)
--   レシピは遊んでいる間に変わらないので、最初に1回だけ作っておく。
--   Recipes don't change during play, so build it once.
----------------------------------------------------------------------

local function recipe_index()
  if mod.index then return mod.index end
  local idx = { by_result = {}, list = {} }
  local all = RecipeRaw.get_all()
  for i = 1, #all do
    local r = all[i]
    try(function()
      local result = r:result():str()
      if result == "" or result == "null" then return end
      local e = { id = r:recipe_id():str(), result = result, name = r:result_name() }
      idx.list[#idx.list + 1] = e
      local same = idx.by_result[result]
      if not same then
        same = {}
        idx.by_result[result] = same
      end
      same[#same + 1] = e
    end)
  end
  mod.index = idx
  return idx
end

----------------------------------------------------------------------
-- 必要なもの（RequirementData）を Lua の表に写す
-- Copy requirements (RequirementData) into plain Lua tables
--
--   groups = {
--     { kind = "comp", alts = { { type = "leather", need = 6 }, ... } },  -- 材料 / components
--     { kind = "tool", alts = { { type = "needle", need = 1 }, ... } },   -- 道具 / tools
--     { kind = "qual", alts = { { type = "CUT", level = 1, count = 1 } } }, -- 道具の品質 / tool qualities
--   }
--   グループどうしは「全部必要」、グループの中の alts は「どれか1つでよい」。
--   All groups are needed; any one of a group's alts is enough.
----------------------------------------------------------------------

local function read_requirements(req)
  local groups = {}

  for _, grp in ipairs(req:get_components()) do
    local alts = {}
    for _, c in ipairs(grp) do
      alts[#alts + 1] = { type = c.type:str(), need = c.count }
    end
    if #alts > 0 then groups[#groups + 1] = { kind = "comp", alts = alts } end
  end

  for _, grp in ipairs(req:get_tools()) do
    local alts = {}
    for _, t in ipairs(grp) do
      local by_charges = t:by_charges()
      -- チャージを使う道具は「量」、そうでなければ道具1つあればよい
      -- Tools that use charges need an amount; others need one tool
      alts[#alts + 1] = {
        type = t.type:str(),
        need = by_charges and t.count or 1,
        charges = by_charges,
      }
    end
    if #alts > 0 then groups[#groups + 1] = { kind = "tool", alts = alts } end
  end

  for _, grp in ipairs(req:get_qualities()) do
    local alts = {}
    for _, q in ipairs(grp) do
      alts[#alts + 1] = { type = q.type:str(), level = q.level, count = q.count }
    end
    if #alts > 0 then groups[#groups + 1] = { kind = "qual", alts = alts } end
  end

  return groups
end

-- レシピの必要なもの（何度も使うので覚えておく）
-- Requirements of a recipe (cached, used many times)
local function recipe_groups(recipe_id_str)
  mod.req_cache = mod.req_cache or {}
  local g = mod.req_cache[recipe_id_str]
  if not g then
    g = try(function()
      return read_requirements(RecipeId.new(recipe_id_str):obj():get_requirements())
    end, {})
    mod.req_cache[recipe_id_str] = g
  end
  return g
end

local function construction_groups(con_id_str)
  return try(function()
    return read_requirements(ConstructionId.new(con_id_str):obj():get_requirements())
  end, {})
end

-- 目標（製作 or 建設）の必要なもの
-- Requirements of the target (recipe or construction)
local function target_groups(run)
  if run.kind == "construction" then
    return construction_groups(run.id)
  end
  return recipe_groups(run.id)
end

----------------------------------------------------------------------
-- 手持ち（＋周り6マス）の確認
-- Check what you have (carried + within 6 tiles)
----------------------------------------------------------------------

-- qty だけ持っているか
-- Whether you have qty of it
local function has_amount(inv, kind, alt, qty)
  return try(function()
    local id = ItypeId.new(alt.type)
    if kind == "tool" then
      if alt.charges then return inv:has_charges(id, qty) end
      return inv:has_tools(id, qty)
    end
    if counted_by_charges(alt.type) then return inv:has_charges(id, qty) end
    return inv:has_components(id, qty)
  end, false) == true
end

-- いくつ持っているか（need までで数えるのをやめる）
-- How many you have (stops counting at need)
--   本体には「数を返す」関数がないので、「n個持っているか」で二分探索する。
--   The game has no "count" function, so binary search with "has n?".
local function count_have(inv, kind, alt)
  local need = alt.need
  if need <= 0 or has_amount(inv, kind, alt, need) then return need end
  local lo, hi = 0, need - 1 -- lo 個は持っている、hi 個より多くは持っていない / have at least lo, not more than hi
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if has_amount(inv, kind, alt, mid) then lo = mid else hi = mid - 1 end
  end
  return lo
end

-- 足りないグループだけを返す。各候補には「あといくつ足りないか（short）」を付ける。
-- Return only the groups that are short; each alt gets how many are still missing (short).
-- 品質は手持ちを調べる手段がないので、ここでは扱わない。
-- Qualities can't be checked against the inventory here, so they are skipped.
local function missing_groups(groups, inv)
  local out = {}
  for _, g in ipairs(groups) do
    if g.kind ~= "qual" then
      local satisfied = false
      local alts = {}
      for _, a in ipairs(g.alts) do
        local got = count_have(inv, g.kind, a)
        if got >= a.need then
          satisfied = true
          break
        end
        alts[#alts + 1] = {
          type = a.type, kind = g.kind, need = a.need, short = a.need - got, charges = a.charges,
        }
      end
      if not satisfied then out[#out + 1] = { kind = g.kind, alts = alts } end
    end
  end
  return out
end

----------------------------------------------------------------------
-- 作り方を探す
-- Find a way to make it
----------------------------------------------------------------------

-- そのレシピを知っているか
-- Whether the recipe is known
local function knows(you, e)
  return try(function() return you:knows_recipe(RecipeId.new(e.id)) end, false) == true
end

-- 今すぐ作れるか（近くの本・仲間が知っているレシピも含む）
-- Whether it can be crafted now (includes nearby books and allies' recipes)
local function can_make_now(you, e)
  return try(function() return you:can_make(RecipeId.new(e.id), 1) end, false) == true
end

-- 作る対象にしてよい候補か（量で使う道具のチャージは作れない）
-- Whether this alt may be crafted (tool charges can't be)
local function craftable_alt(a)
  return not (a.kind == "tool" and a.charges)
end

--[[
  足りない部品のうち、「今すぐ作れるもの」を1つ探す。

    1. 足りない部品そのもので、今すぐ作れるレシピがあればそれ
    2. なければ、知っているレシピの材料（部品の部品）へさかのぼって同じことをする

  ctx.visiting … 今たどっている途中の品（輪になって同じ品に戻るのを防ぐ）
  ctx.dead     … この探索で作り方が見つからなかった品（同じ品を何度も調べない）
  ctx.budget   … 調べてよいレシピの残り数
  見つかれば { recipe = レシピ, item = 何のために作るか } を返す。

  Find one missing part that can be made right now:
    1. a recipe for the missing part itself that can be crafted now
    2. otherwise, do the same for the components of known recipes
  ctx.visiting: items on the current path (prevents loops)
  ctx.dead:     items with no way to make found in this search
  ctx.budget:   how many more recipes may be checked
  Returns { recipe = recipe, item = what it is made for } when found.
]]
local function find_step(groups, inv, you, depth, ctx)
  if depth > CFG.max_depth then return nil end
  local idx = recipe_index()
  local missing = missing_groups(groups, inv)

  -- 1. 今すぐ作れるもの
  -- 1. Something that can be made right now
  for _, g in ipairs(missing) do
    for _, a in ipairs(g.alts) do
      if craftable_alt(a) and not ctx.visiting[a.type] then
        for _, e in ipairs(idx.by_result[a.type] or {}) do
          ctx.budget = ctx.budget - 1
          if ctx.budget < 0 then return nil end
          if can_make_now(you, e) then
            return { recipe = e, item = a }
          end
        end
      end
    end
  end

  -- 2. 部品の部品へさかのぼる
  -- 2. Go back to the parts of parts
  for _, g in ipairs(missing) do
    for _, a in ipairs(g.alts) do
      if craftable_alt(a) and not ctx.visiting[a.type] and not ctx.dead[a.type] then
        ctx.visiting[a.type] = true
        for _, e in ipairs(idx.by_result[a.type] or {}) do
          if knows(you, e) then
            local found = find_step(recipe_groups(e.id), inv, you, depth + 1, ctx)
            if found then
              ctx.visiting[a.type] = nil
              return found
            end
          end
          if ctx.budget < 0 then
            ctx.visiting[a.type] = nil
            return nil
          end
        end
        ctx.visiting[a.type] = nil
        ctx.dead[a.type] = true
      end
    end
  end

  return nil
end

local function new_search(root_type, budget)
  local ctx = { visiting = {}, dead = {}, budget = budget or CFG.search_budget }
  -- 目標の品そのものを材料にするレシピはたどらない
  -- Don't follow recipes that use the target item itself
  if root_type then ctx.visiting[root_type] = true end
  return ctx
end

----------------------------------------------------------------------
-- 表示用の文
-- Text for display
----------------------------------------------------------------------

-- 候補1つ：「革 ×6」「糸 ×20」
-- One alt: "leather x6", "thread x20"
local function alt_label(a)
  if a.kind == "tool" and not a.charges then
    return item_name(a.type, 1)
  end
  return string.format("%s ×%d", item_name(a.type, a.short or a.need), a.short or a.need)
end

-- グループ：「革 ×6 / なめし革 ×6」
-- A group: "leather x6 / tanned leather x6"
local function group_label(g)
  local parts = {}
  for i, a in ipairs(g.alts) do
    if i > 3 then
      parts[#parts + 1] = string.format(T("(+%d more)"), #g.alts - 3)
      break
    end
    parts[#parts + 1] = alt_label(a)
  end
  return table.concat(parts, " / ")
end

-- 品質の一覧：「切断 1 / 打撃 2」
-- List of qualities: "cutting 1 / hammering 2"
local function qualities_label(groups)
  local parts = {}
  for _, g in ipairs(groups) do
    if g.kind == "qual" then
      for _, q in ipairs(g.alts) do
        parts[#parts + 1] = string.format("%s %d", quality_name(q.type), q.level)
      end
    end
  end
  return table.concat(parts, " / ")
end

----------------------------------------------------------------------
-- 実行（1回の製作が終わるたびに次を決める）
-- Running (decide the next step after each craft)
--
--   mod.run = {
--     kind = "recipe" | "construction", id = 目標のID, label = 表示名,
--     root = 目標の完成品（製作のとき）,
--     waiting = 製作中のレシピID, finished = その製作が終わったか,
--     final = 目標そのものを作っているところか,
--     crafts = 部品を作った回数, same = { レシピID = 回数 },
--   }
--   (kind, target id, label, root = result item of a recipe target,
--    waiting = recipe being crafted, finished = whether it finished,
--    final = crafting the target itself, crafts = parts crafted,
--    same = times crafted per recipe)
----------------------------------------------------------------------

local function stop(msg)
  if msg then say(msg) end
  mod.run = nil
end

-- 行き詰まったとき、何が足りないかを知らせる
-- When stuck, tell the player what is missing
local function report_stuck(run, inv)
  local groups = target_groups(run)
  local missing = missing_groups(groups, inv)
  local lines = {}
  for _, g in ipairs(missing) do
    lines[#lines + 1] = group_label(g)
  end
  if #lines > 0 then
    stop(string.format(T("Can't make the missing parts for %s.  Still missing: %s"),
      run.label, table.concat(lines, ", ")))
  else
    -- 材料と道具はそろっている → 品質・技能・知らないレシピなど
    -- Components and tools are there -> qualities, skills, unknown recipe, ...
    local q = qualities_label(groups)
    if q ~= "" then
      stop(string.format(T("The parts for %s are ready, but it still can't be made.  Check the tools (needed qualities: %s), skills and the recipe."),
        run.label, q))
    else
      stop(string.format(T("The parts for %s are ready, but it still can't be made.  Check your tools, skills and the recipe."),
        run.label))
    end
  end
end

-- 次の1回を決めて始める
-- Decide and start the next craft
local function next_step(run, you)
  you:invalidate_crafting_inventory()
  local inv = you:crafting_inventory()

  -- 目標がもう作れる（建てられる）か
  -- Can the target be crafted (built) already?
  if run.kind == "recipe" then
    if try(function() return you:can_make(RecipeId.new(run.id), 1) end, false) then
      say(string.format(T("All parts are ready.  Crafting %s."), run.label))
      if try(function() return you:make_craft(RecipeId.new(run.id), 1) end, false) then
        run.waiting = run.id
        run.finished = false
        run.final = true
      else
        stop(string.format(T("Could not start crafting %s."), run.label))
      end
      return
    end
  else
    if try(function() return you:can_build(ConstructionId.new(run.id)) end, false) then
      stop(string.format(T("All materials for %s are ready.  Build it from the construction menu."), run.label))
      return
    end
  end

  if run.crafts >= CFG.max_crafts then
    stop(string.format(T("Stopped after making parts %d times.  Choose \"Continue\" in the menu to go on."), run.crafts))
    return
  end

  local step = find_step(target_groups(run), inv, you, 1, new_search(run.root))
  if not step then
    report_stuck(run, inv)
    return
  end

  -- 同じ部品ばかり作り続けていないか
  -- Stop if the same part is made over and over
  local id = step.recipe.id
  run.same[id] = (run.same[id] or 0) + 1
  if run.same[id] > CFG.max_same then
    stop(string.format(T("Stopped: kept making %s over and over."), step.recipe.name))
    return
  end

  say(string.format(T("Making %s for %s."), step.recipe.name, run.label))
  if try(function() return you:make_craft(RecipeId.new(id), 1) end, false) then
    run.waiting = id
    run.finished = false
    run.crafts = run.crafts + 1
  else
    stop(string.format(T("Could not start crafting %s."), step.recipe.name))
  end
end

-- 毎ターン呼ばれる（preload.lua から）
-- Called every turn (from preload.lua)
function mod.tick()
  local run = mod.run
  if not run then return end
  local you = gapi.get_avatar()
  local act = activity_of(you)

  -- 製作中は終わるのを待つ
  -- Wait while crafting
  if act == ACT_CRAFT then return end

  if run.waiting then
    -- 製作が終わった知らせ（on_craft_result）が来ないまま止まった＝中断か失敗
    -- Stopped without on_craft_result = interrupted or failed
    if not run.finished then
      mod.last = { kind = run.kind, id = run.id, label = run.label, root = run.root }
      stop(T("Crafting was interrupted or failed.  If there is an unfinished item, finish it first, then choose \"Continue\" in the menu."))
      return
    end
    run.waiting = nil
    if run.final then
      stop(string.format(T("Finished crafting %s."), run.label))
      return
    end
  end

  -- 本を読むなど、別のことをしている間は待つ
  -- Wait while doing something else (reading, ...)
  if act ~= "" then return end

  next_step(run, you)
end

-- 製作が1回終わったとき（preload.lua から）
-- When one craft finishes (from preload.lua)
function mod.on_craft_result(params)
  local run = mod.run
  if not run or not run.waiting then return end
  -- 製作メニューの表示のための呼び出しは関係ない
  -- Ignore calls made for the crafting menu display
  if params.crafting_menu then return end
  local mine = try(function()
    return params.crafter:is_avatar() and params.recipe:recipe_id():str() == run.waiting
  end, false)
  if mine then run.finished = true end
end

local function start(target)
  mod.run = {
    kind = target.kind, id = target.id, label = target.label, root = target.root,
    crafts = 0, same = {},
  }
  mod.last = { kind = target.kind, id = target.id, label = target.label, root = target.root }
  say(string.format(T("Started gathering the parts for %s."), target.label))
  -- すぐに1回目を始める（作っている間に次を決める）
  -- Start the first craft now (the next one is decided after it)
  next_step(mod.run, gapi.get_avatar())
end

----------------------------------------------------------------------
-- 画面
-- Screens
----------------------------------------------------------------------

local function pick(title, text, entries)
  local ui = UiList.new()
  ui:title(title)
  if text and text ~= "" then ui:text(text) end
  for i, e in ipairs(entries) do
    ui:add(i, e)
  end
  local r = ui:query()
  if type(r) ~= "number" or r < 1 or r > #entries then return nil end
  return r
end

local function ask_text(title, desc)
  local pop = PopupInputStr.new()
  pop:title(title)
  if desc then pop:desc(desc) end
  return string.lower(try(function() return pop:query_str() end, "") or "")
end

local function ask_yes(text)
  local pop = QueryPopup.new()
  pop:message(text)
  return try(function() return pop:query_yn() end, "NO") == "YES"
end

local function popup(text)
  local pop = QueryPopup.new()
  pop:message(text)
  pop:allow_any_key(true)
  try(function() pop:query() end)
end

-- 実行する前の確認画面
-- Confirmation screen before starting
local function confirm(target)
  local you = gapi.get_avatar()
  you:invalidate_crafting_inventory()
  local inv = you:crafting_inventory()
  local groups = target_groups(target)
  local missing = missing_groups(groups, inv)

  local lines = { string.format(T("Target: %s"), target.label), "" }
  if #missing == 0 then
    lines[#lines + 1] = T("Nothing is missing.")
  else
    lines[#lines + 1] = T("Missing:")
    for i, g in ipairs(missing) do
      if i > CFG.max_preview then
        lines[#lines + 1] = string.format(T("  ...and %d more"), #missing - CFG.max_preview)
        break
      end
      -- このグループだけを対象に作り方を探して、作れそうかを添える
      -- Search only for this group and show whether it can be made
      local found = find_step({ g }, inv, you, 1, new_search(target.root, CFG.preview_budget))
      local mark = found and T("can make") or T("no way to make")
      lines[#lines + 1] = string.format("  %s  [%s]", group_label(g), mark)
    end
  end
  local q = qualities_label(groups)
  if q ~= "" then
    lines[#lines + 1] = ""
    lines[#lines + 1] = string.format(T("Tool qualities needed (not crafted automatically): %s"), q)
  end
  lines[#lines + 1] = ""
  lines[#lines + 1] = T("Start?  Parts are crafted one at a time, like crafting from the menu.")
  return ask_yes(table.concat(lines, "\n"))
end

-- 作りたい品を選ぶ
-- Choose an item to craft
local function choose_recipe()
  local q = ask_text(T("Item to craft"),
    T("Type part of the item name.  Leave it empty to list every recipe you know."))
  local you = gapi.get_avatar()
  local found = {}
  for _, e in ipairs(recipe_index().list) do
    if (q == "" or string.find(string.lower(e.name), q, 1, true)) and knows(you, e) then
      found[#found + 1] = e
      if #found >= CFG.max_list then break end
    end
  end
  if #found == 0 then
    popup(T("No matching recipes.  Only recipes you know are listed."))
    return nil
  end
  table.sort(found, function(a, b) return a.name < b.name end)

  local entries = {}
  for i, e in ipairs(found) do
    entries[i] = e.name
    if can_make_now(you, e) then
      entries[i] = entries[i] .. "  " .. T("[ready]")
    end
  end
  local r = pick(T("Item to craft"), nil, entries)
  if not r then return nil end
  local e = found[r]
  return { kind = "recipe", id = e.id, label = e.name, root = e.result }
end

-- 建設したいものを選ぶ
-- Choose something to build
--   同じ見出し（例：「木の壁を建てる」）に材料違いがあるので、材料も添えて並べる。
--   One name (e.g. "Build Wood Wall") has several material variants, so materials are shown too.
local function choose_construction()
  local q = ask_text(T("Construction"),
    T("Type part of the construction name.  Leave it empty to list everything."))
  local you = gapi.get_avatar()
  local found = {}
  local ids = ConstructionRaw.get_all()
  for i = 1, #ids do
    local info = try(function()
      local cid = ids[i]
      local c = cid:obj()
      local name = c:group_name()
      if name == "" then return nil end
      if q ~= "" and not string.find(string.lower(name), q, 1, true) then return nil end
      local groups = read_requirements(c:get_requirements())
      -- 材料の最初の2グループを見分け用に添える
      -- Show the first two component groups to tell variants apart
      local hint = {}
      for _, g in ipairs(groups) do
        if g.kind == "comp" then
          hint[#hint + 1] = group_label(g)
          if #hint >= 2 then break end
        end
      end
      return {
        id = cid:str(),
        label = name,
        hint = table.concat(hint, ", "),
        skills = you:meets_construction_skills(cid),
        ready = you:can_build(cid),
      }
    end, nil)
    if info then
      found[#found + 1] = info
      if #found >= CFG.max_list then break end
    end
  end
  if #found == 0 then
    popup(T("No matching constructions."))
    return nil
  end

  local entries = {}
  for i, f in ipairs(found) do
    local s = f.label
    if f.hint ~= "" then s = s .. "  (" .. f.hint .. ")" end
    if f.ready then
      s = s .. "  " .. T("[ready]")
    elseif not f.skills then
      s = s .. "  " .. T("(skill too low)")
    end
    entries[i] = s
  end
  local r = pick(T("Construction"), nil, entries)
  if not r then return nil end
  local f = found[r]
  return { kind = "construction", id = f.id, label = f.label }
end

-- アクションメニューから開く
-- Opened from the action menu
function mod.open_menu()
  if not has_recipe_api() then
    popup(T("This mod needs a newer game version (Lua crafting functions from PR #10477).  It does nothing on this version."))
    return
  end

  if mod.run then
    local r = pick(T("Craft missing parts"),
      string.format(T("Now gathering the parts for %s."), mod.run.label),
      { T("Stop") })
    if r == 1 then
      mod.last = { kind = mod.run.kind, id = mod.run.id, label = mod.run.label, root = mod.run.root }
      stop(T("Stopped gathering parts."))
    end
    return
  end

  local entries, actions = {}, {}
  entries[#entries + 1] = T("Choose an item to craft")
  actions[#actions + 1] = "recipe"
  if has_construction_api() then
    entries[#entries + 1] = T("Choose something to build")
    actions[#actions + 1] = "construction"
  end
  if mod.last then
    entries[#entries + 1] = string.format(T("Continue: %s"), mod.last.label)
    actions[#actions + 1] = "continue"
  end

  local r = pick(T("Craft missing parts"),
    T("Crafts the parts you are missing, using items you carry and within 6 tiles."), entries)
  if not r then return end

  local target
  if actions[r] == "recipe" then
    target = choose_recipe()
  elseif actions[r] == "construction" then
    target = choose_construction()
  else
    target = mod.last
  end
  if not target then return end
  if confirm(target) then
    start(target)
  end
end
