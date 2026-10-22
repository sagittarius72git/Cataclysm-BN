--[[
  Craft Missing Parts - preload.lua
  フック・アクションメニューの登録だけを置く。実装は main.lua 側。
  Only registers hooks and the action menu entry; the logic is in main.lua.

  Author: sagittarius72
  Assisted-by: Claude:claude-opus-5-5
]]

gdebug.log_info("CMP: preload.")

local mod = game.mod_runtime[game.current_mod]

-- 毎ターン：製作が1回終わったら、次に作る部品を決めて始める
--   （何も実行していないときはすぐ戻るので軽い）
-- Every turn: after a craft finishes, decide and start the next part
--   (returns at once when nothing is running, so it is cheap)
gapi.add_on_every_x_hook(TimeDuration.from_turns(1), function()
  if mod.tick then
    mod.tick()
  end
end)

-- 製作が1回終わったとき：中断や失敗と見分けるために使う
--   古い本体にないフック名を登録するとエラーが出るので、あるときだけ登録する
-- When a craft finishes: used to tell it apart from interruption or failure
--   Registered only if the hook exists (older games error on unknown hooks)
if game.hooks["on_craft_result"] ~= nil then
  game.add_hook("on_craft_result", function(params)
    if mod.on_craft_result then
      mod.on_craft_result(params)
    end
  end)
end

-- アクションメニューの「足りない部品を作る」
-- "Craft missing parts" entry in the action menu
gapi.register_action_menu_entry({
  id = "craft_missing_parts_open",
  name = locale.gettext("Craft missing parts"),
  category = "misc",
  fn = function()
    if mod.open_menu then
      mod.open_menu()
    end
  end,
})
