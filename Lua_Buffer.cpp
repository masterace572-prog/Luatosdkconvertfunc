#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <cstdint>
#include <android/log.h>

#define KAUSHIK_NAME "SHADOW"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "LUA_46", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "LUA_46", __VA_ARGS__)

// ==================== OFFSETLER (4.6 GÜNCEL) ====================
#define LuaLoadBuffer_Offset  0xB51186C
#define LuaPCall_Offset       0xB4ED430
#define LuaLoad_Offset        0xB4ED430



const char* mortarAim_script = R"MORTAR(
-- ============================================================
-- MORTAR AUTO AIM (standalone)
-- ============================================================
local MortarGameplayData = nil
local bMortarDataReady   = false
local bMortarActive      = false
local MortarLockedTarget = nil
local MortarLastAimRot   = nil

local MortarConfig = {
    AimInterval     = 0.03,   -- tick rate (33 Hz)
    MaxRange        = 600.0,  -- meters
    FOV             = 20.0,   -- degrees
    BaseGravity     = 980.0,
    SwipeBreakAngle = 5.0,    -- degrees of yaw change to break lock
}

-- ---- Notify (uses the game's tip system, no external dependency) ----
local function MortarNotify(msg)
    pcall(function()
        local ok, Tips = pcall(require, "GameLua.Mod.BaseMod.Common.UI.InGameTipsTools")
        if ok and Tips and Tips.BattleNormalTips then
            Tips.BattleNormalTips("[MortarAim] " .. tostring(msg), 2, 3)
        end
    end)
    print("[MortarAim] " .. tostring(msg))
end

-- ---- GameplayData lazy load ----
local function MortarEnsureGameplayData()
    if bMortarDataReady then return true end
    local ok, GD = pcall(require, "GameLua.GameCore.Data.GameplayData")
    if ok and GD then
        MortarGameplayData = GD
        bMortarDataReady = true
    end
    return bMortarDataReady
end

local function MortarNormalizeAngle(angle)
    while angle >  180 do angle = angle - 360 end
    while angle < -180 do angle = angle + 360 end
    return angle
end

local function MortarAtan2(y, x)
    if x > 0 then return math.atan(y / x)
    elseif x < 0 and y >= 0 then return math.atan(y / x) + math.pi
    elseif x < 0 and y < 0  then return math.atan(y / x) - math.pi
    elseif x == 0 and y > 0 then return  math.pi / 2
    elseif x == 0 and y < 0 then return -math.pi / 2
    else return 0 end
end

local function MortarIsEnemyTarget(a, b)
    if not (slua.isValid(a) and slua.isValid(b)) then return false end
    if a == b then return false end
    if a.PlayerKey ~= nil and b.PlayerKey ~= nil and a.PlayerKey == b.PlayerKey then
        return false
    end
    local ta, tb = -1, -2
    pcall(function()
        ta = a.TeamID or (a.PlayerState and a.PlayerState.TeamNum) or -1
        tb = b.TeamID or (b.PlayerState and b.PlayerState.TeamNum) or -2
    end)
    return ta ~= tb
end

local function MortarIsPawnAlive(p)
    if not slua.isValid(p) then return false end
    local dead = false
    pcall(function() dead = p.bDead == true end)
    return not dead
end

-- bUseHeadSocket: when true (aiming), target the head; else target the body offset
local function MortarGetTargetLocation(pawn, bUseHeadSocket)
    local loc = nil
    pcall(function()
        if bUseHeadSocket then
            if slua.isValid(pawn.Mesh) then
                loc = pawn.Mesh:GetSocketLocation("head")
            end
        else
            loc = pawn:K2_GetActorLocation()
            if loc then
                loc.Z = loc.Z - 85.0
                local right = pawn:GetActorRightVector()
                if right then
                    loc.X = loc.X + right.X * 50.0
                    loc.Y = loc.Y + right.Y * 50.0
                end
            end
        end
    end)
    return loc
end

-- Ballistic solve: returns pitch (degrees) and out_of_range flag
local function MortarSolvePitch(dist, hDiff, speed, gravity)
    if dist < 1.0 then dist = 1.0 end
    local v2 = speed * speed
    local v4 = v2 * v2
    local disc = v4 - gravity * (gravity * dist * dist + 2 * hDiff * v2)
    if disc < 0 then
        local pitch = math.atan(v2 / (gravity * dist)) * (180.0 / math.pi)
        pitch = math.max(45.0, math.min(88.0, pitch))
        return pitch, true
    end
    local root = math.sqrt(disc)
    local pitch = math.atan((v2 + root) / (gravity * dist)) * (180.0 / math.pi)
    pitch = math.max(45.0, math.min(88.0, pitch))
    return pitch, false
end

-- Maps ballistic pitch to UE control pitch (-60..30 range)
local function MortarPitchToControlPitch(pitch)
    local ctrl = -60.0 + (pitch - 45.0) * 2.0930232558139537
    ctrl = math.max(-60.0, math.min(30.0, ctrl))
    return ctrl
end

local function MortarFindBestTarget(selfPawn, controller, bUseHeadSocket)
    local camMgr = nil
    pcall(function() camMgr = controller.PlayerCameraManager end)
    if not slua.isValid(camMgr) then return nil end

    local camLoc = camMgr:GetCameraLocation()
    local camRot = camMgr:GetCameraRotation()

    local candidates = {}
    pcall(function()
        local list = MortarGameplayData.GetAllPlayerCharacters()
        if list then
            for _, c in pairs(list) do
                if slua.isValid(c) then candidates[c] = true end
            end
        end
    end)

    local best, bestAngle = nil, MortarConfig.FOV
    for pawn in pairs(candidates) do
        if MortarIsPawnAlive(pawn) and MortarIsEnemyTarget(selfPawn, pawn) then
            local tloc = MortarGetTargetLocation(pawn, bUseHeadSocket)
            if tloc then
                local dx, dy = tloc.X - camLoc.X, tloc.Y - camLoc.Y
                local d2 = math.sqrt(dx * dx + dy * dy)
                if d2 / 100.0 <= MortarConfig.MaxRange then
                    local yaw = MortarAtan2(dy, dx) * (180.0 / math.pi)
                    local diff = math.abs(MortarNormalizeAngle(yaw - camRot.Yaw))
                    if diff <= MortarConfig.FOV and diff < bestAngle then
                        bestAngle = diff
                        best = pawn
                    end
                end
            end
        end
    end
    return best
end

local function MortarUpdateAim()
    if not MortarGameplayData then return end

    local selfPawn, controller = nil, nil
    pcall(function() selfPawn   = MortarGameplayData.GetPlayerCharacter()  end)
    pcall(function() controller = MortarGameplayData.GetPlayerController() end)
    if not (slua.isValid(selfPawn) and slua.isValid(controller)) then return end

    local camMgr = nil
    pcall(function() camMgr = controller.PlayerCameraManager end)
    if not slua.isValid(camMgr) then return end

    local weapon = nil
    pcall(function()
        weapon = selfPawn.CurrentWeapon or selfPawn:GetCurrentWeapon()
    end)
    if not slua.isValid(weapon) then return end

    -- Only proceed when in the mortar-aiming state
    local mortState, aiming = 0, false
    pcall(function()
        mortState = weapon.MortarState
        aiming    = weapon.MortarAimState == 1
    end)
    if mortState ~= 2 then return end

    local camRot = camMgr:GetCameraRotation()
    local selfLoc = selfPawn:K2_GetActorLocation()

    -- Break lock if player swipes the camera beyond threshold
    if MortarLockedTarget and MortarLastAimRot then
        local dYaw = math.abs(MortarNormalizeAngle(camRot.Yaw - MortarLastAimRot.Yaw))
        if dYaw > MortarConfig.SwipeBreakAngle then
            MortarLockedTarget = nil
            MortarNotify("Target unlocked (manual swipe)")
        end
    end

    -- Validate existing lock
    if MortarLockedTarget then
        if MortarIsPawnAlive(MortarLockedTarget) and
           MortarIsEnemyTarget(selfPawn, MortarLockedTarget) then
            local lloc = MortarGetTargetLocation(MortarLockedTarget, aiming)
            if lloc then
                local ld = math.sqrt((lloc.X - selfLoc.X)^2 + (lloc.Y - selfLoc.Y)^2) / 100.0
                if ld > MortarConfig.MaxRange then
                    MortarLockedTarget = nil
                end
            else
                MortarLockedTarget = nil
            end
        else
            MortarLockedTarget = nil
        end
    end

    -- Acquire new target
    if not MortarLockedTarget then
        MortarLockedTarget = MortarFindBestTarget(selfPawn, controller, aiming)
        if MortarLockedTarget then
            MortarNotify("Target acquired")
        end
    end

    if not MortarLockedTarget then
        MortarLastAimRot = nil
        return
    end

    -- Compute ballistic solution
    local aimLoc = MortarGetTargetLocation(MortarLockedTarget, aiming)

    local muzzle = nil
    pcall(function() muzzle = weapon:K2_GetActorLocation() end)
    local ox = muzzle and muzzle.X or selfLoc.X
    local oy = muzzle and muzzle.Y or selfLoc.Y
    local oz = muzzle and muzzle.Z or selfLoc.Z

    local dx, dy, dz = aimLoc.X - ox, aimLoc.Y - oy, aimLoc.Z - oz
    local d2 = math.sqrt(dx * dx + dy * dy)

    local bulletSpeed = 9070.0
    local gravityScale = 2.8
    pcall(function()
        if weapon.GetBulletFireSpeedFromEntity then
            bulletSpeed = weapon:GetBulletFireSpeedFromEntity()
        else
            bulletSpeed = aiming and 12520.0 or 9070.0
        end
        if slua.isValid(weapon.ShootWeaponEntity)
            and weapon.ShootWeaponEntity.LaunchGravityScale then
            gravityScale = weapon.ShootWeaponEntity.LaunchGravityScale
        else
            gravityScale = aiming and 4.0 or 2.8
        end
    end)

    local gravity = MortarConfig.BaseGravity * gravityScale
    local pitch, outOfRange = MortarSolvePitch(d2, dz, bulletSpeed, gravity)

    if outOfRange and not aiming then
        if not _G.NTH_LastWarnTime or (os.clock() - _G.NTH_LastWarnTime) > 2.0 then
            MortarNotify("Target too far - aim down sight to extend range")
            _G.NTH_LastWarnTime = os.clock()
        end
    end

    local ctrlPitch = MortarPitchToControlPitch(pitch)
    local targetYaw = MortarAtan2(dy, dx) * (180.0 / math.pi)
    local aimRot    = FRotator(ctrlPitch, targetYaw, 0)

    -- Apply to pawn + controller
    pcall(function()
        if slua.isValid(camMgr) then
            camMgr.bLimitViewPitch = false
            camMgr.bLimitViewYaw   = false
            camMgr.ViewPitchMin    = -89.9
            camMgr.ViewPitchMax    =  89.9
        end
        selfPawn:K2_SetActorRotation(FRotator(0, targetYaw, 0), false)
        selfPawn.BaseAimRotation    = aimRot
        controller.ControlRotation  = aimRot
        MortarLastAimRot            = aimRot
    end)
end

local MortarTickCount = 0
local function MortarOnTick()
    MortarTickCount = MortarTickCount + 1
    pcall(function()
        if not bMortarDataReady then
            MortarEnsureGameplayData()
            return
        end

        local selfPawn = nil
        pcall(function() selfPawn = MortarGameplayData.GetPlayerCharacter() end)
        if not slua.isValid(selfPawn) then return end

        local weapon = nil
        pcall(function()
            weapon = selfPawn.CurrentWeapon or selfPawn:GetCurrentWeapon()
        end)

        local isMortar = false
        pcall(function()
            isMortar = string.find(string.lower(tostring(weapon)), "mortar") ~= nil
        end)

        if not isMortar then
            if bMortarActive then
                bMortarActive      = false
                MortarLockedTarget = nil
                MortarLastAimRot   = nil
                MortarNotify("Disabled - not holding mortar")
            end
            return
        end

        if not bMortarActive then
            bMortarActive = true
            MortarNotify("Enabled - mortar detected")
        end

        MortarUpdateAim()
    end)
end

-- ---- Start the loop ----
local function MortarStart()
    local ticker = package.loaded["common.time_ticker"]
    if not ticker then
        local ok, t = pcall(require, "common.time_ticker")
        if ok then ticker = t end
    end
    if not (ticker and ticker.AddTimerLoop) then
        MortarNotify("ERROR: time_ticker missing")
        return false
    end
    ticker.AddTimerLoop(0, MortarOnTick, -1, MortarConfig.AimInterval)
    MortarNotify("Mortar Auto Aim started")
    return true
end

MortarStart()
)MORTAR";

const char* GuestLoginLuaScript = R"GUEST(
local function guestLog(message)
  pcall(function()
    if log then
      log("[GuestLoginLua] " .. tostring(message))
    else
      print("[GuestLoginLua] " .. tostring(message))
    end
  end)
end

local unpackArgs = table.unpack or unpack

local function safeRequire(name)
  local ok, value = pcall(require, name)
  if ok then return value end
  return nil
end

local function sendResult(callback, value)
  if not callback or not callback.SendResult then return end
  local result = {value = value}
  if json and json.encode then result = json.encode(result) end
  callback:SendResult(result)
end

local function sendError(callback, message)
  if callback and callback.SendError then
    callback:SendError(message)
  else
    guestLog(message)
  end
end

local function later(sec, fn)
  if type(fn) ~= "function" then return false end
  sec = tonumber(sec) or 1
  if _G.SetTimer and pcall(_G.SetTimer, sec, fn) then return true end
  if _G.Mytimer_ticker then
    if _G.Mytimer_ticker.AddTimer and pcall(_G.Mytimer_ticker.AddTimer, sec, fn) then return true end
    if _G.Mytimer_ticker.AddTimerLoop and pcall(_G.Mytimer_ticker.AddTimerLoop, sec, fn, 1, sec) then return true end
  end
  local ticker = safeRequire("common.time_ticker")
  if ticker then
    if ticker.AddTimer and pcall(ticker.AddTimer, sec, fn) then return true end
    if ticker.AddTimerOnce and pcall(ticker.AddTimerOnce, sec, fn) then return true end
    if ticker.AddTimerLoop and pcall(ticker.AddTimerLoop, sec, fn, 1, sec) then return true end
  end
  return false
end

local function guestChannel()
  if type(ShareSource) == "table" and ShareSource.Guest then
    return ShareSource.Guest
  end
  return "guest"
end

local function moduleManager()
  if type(ModuleManager) == "table" then return ModuleManager end
  return safeRequire("client.module_framework.ModuleManager")
end

local function loginModule()
  local mm = moduleManager()
  if not mm or not mm.GetModule or not mm.LobbyModuleConfig then return nil end
  local ok, module = pcall(function()
    return mm.GetModule(mm.LobbyModuleConfig.login_module)
  end)
  if ok then return module end
  return nil
end

local function acceptGuestWarning()
  local login = loginModule()
  if not login then return false end
  pcall(function()
    login.playerData = login.playerData or {}
    login.playerData.GuestWarningAccepted = true
  end)
  if type(login.SetPlayerData) == "function" then
    pcall(function() login:SetPlayerData("GuestWarningAccepted", true) end)
  end
  return true
end

local function withMatrixDeviceName(fn)
  local mm = moduleManager()
  local deviceModule
  if mm and mm.GetModule and mm.LobbyModuleConfig then
    local okDevice
    okDevice, deviceModule = pcall(function()
      return mm.GetModule(mm.LobbyModuleConfig.device_module)
    end)
    if not okDevice then deviceModule = nil end
  end

  local changed, oldName = false, nil
  if deviceModule then
    local okName
    okName, oldName = pcall(function() return deviceModule.sDeviceName end)
    if okName then
      local current = tostring(oldName or "")
      if not string.find(current, "matrix", 1, true) then
        changed = pcall(function()
          deviceModule.sDeviceName = current ~= "" and (current .. " matrix") or "matrix"
        end)
      end
    end
  end

  local ok, r1, r2, r3 = pcall(fn)
  if changed then
    pcall(function() deviceModule.sDeviceName = oldName end)
  end
  if ok then return true, r1, r2, r3 end
  guestLog("matrix wrapper error: " .. tostring(r1))
  return false
end

local function hookLoginUIClass()
  if _G.GuestLoginClassHooked then return true end
  local modules = {
    "client.slua.umg.NewLogin.Login_UIBP",
    "client.slua.umg.Login.Login_UIBP",
  }

  for _, moduleName in ipairs(modules) do
    local loginClass = safeRequire(moduleName)
    local impl = loginClass and (loginClass.__inner_impl or loginClass)
    if type(impl) == "table" then
      if type(impl.UpdateLoginButtons) == "function" and not impl.__GuestLoginUpdateHooked then
        local oldUpdate = impl.UpdateLoginButtons
        impl.UpdateLoginButtons = function(self, ...)
        local args = {...}
        local ok, r1, r2, r3 = withMatrixDeviceName(function()
            return oldUpdate(self, unpackArgs(args))
          end)
          if ok then return r1, r2, r3 end
        end
        impl.__GuestLoginUpdateHooked = true
      end

      if type(impl.OnClickGuestLogin) == "function" and not impl.__GuestLoginClickHooked then
        local oldGuestClick = impl.OnClickGuestLogin
        impl.OnClickGuestLogin = function(self, ...)
          acceptGuestWarning()
          return oldGuestClick(self, ...)
        end
        impl.__GuestLoginClickHooked = true
      elseif type(impl.OnClickLogin) == "function" and not impl.__GuestLoginClickHooked then
        impl.OnClickGuestLogin = function(self)
          acceptGuestWarning()
          return self:OnClickLogin(guestChannel())
        end
        impl.__GuestLoginClickHooked = true
      end

      _G.GuestLoginClassHooked = true
      guestLog("login UI class hooked: " .. moduleName)
      return true
    end
  end
  return false
end

local function loginUI()
  local UIManager = safeRequire("client.slua_ui_framework.manager")
  if not UIManager or not UIManager.UI_Config or not UIManager.GetUI then return nil end
  local candidates = {}
  if UIManager.UI_Config.Login_UIBP then candidates[#candidates + 1] = UIManager.UI_Config.Login_UIBP end
  if UIManager.UI_Config.NewLogin_UIBP then candidates[#candidates + 1] = UIManager.UI_Config.NewLogin_UIBP end
  for _, cfg in ipairs(candidates) do
    if cfg then
      local ok, ui = pcall(function() return UIManager.GetUI(cfg) end)
      if ok and ui then return ui end
    end
  end
  return nil
end

local function refreshGuestButton()
  hookLoginUIClass()
  local ui = loginUI()
  if not ui or type(ui.UpdateLoginButtons) ~= "function" then return false end
  local ok = withMatrixDeviceName(function()
    return ui:UpdateLoginButtons()
  end)
  return ok == true
end

local function doGuestLogin(callback, silent)
  hookLoginUIClass()
  acceptGuestWarning()
  local ui = loginUI()
  if ui then
    if type(ui.OnClickGuestLogin) == "function" then
      local ok = pcall(function() ui:OnClickGuestLogin() end)
      if ok then sendResult(callback, true); return true end
    end
    if type(ui.OnClickLogin) == "function" then
      local ok = pcall(function() ui:OnClickLogin(guestChannel()) end)
      if ok then sendResult(callback, true); return true end
    end
  end
  if not silent then sendError(callback, "guest login UI not ready") end
  return false
end

local function doGuestLoginWithRetry(callback)
  if doGuestLogin(callback, true) then return true end
  local attempts = 0
  local function tick()
    attempts = attempts + 1
    if doGuestLogin(callback, true) then return end
    if attempts < 15 and later(1, tick) then return end
    sendError(callback, "guest login UI not ready")
  end
  if later(1, tick) then return false end
  sendError(callback, "guest login UI not ready")
  return false
end

local function registerCommands()
  local ok, err = pcall(function()
    local CommandList = require("reautomatic.commands.command_list")
    function CommandList.LoginByGuest(arg, callback)
      doGuestLoginWithRetry(callback)
    end
    function CommandList.GuestLogin(arg, callback)
      doGuestLoginWithRetry(callback)
    end
    function CommandList.ShowGuestLogin(arg, callback)
      sendResult(callback, refreshGuestButton())
    end
  end)
  if ok then
    _G.GuestLoginCommandsRegistered = true
    guestLog("commands registered")
    return true
  end
  guestLog("command registration failed: " .. tostring(err))
  return false
end

local function registerCommandsWithRetry()
  if registerCommands() then return end
  local attempts = 0
  local function tick()
    attempts = attempts + 1
    if registerCommands() then return end
    if attempts < 20 then later(2, tick) end
  end
  later(2, tick)
end

local function hookLoginClassWithRetry()
  if hookLoginUIClass() then return end
  local attempts = 0
  local function tick()
    attempts = attempts + 1
    if hookLoginUIClass() then return end
    if attempts < 30 then later(1, tick) end
  end
  later(1, tick)
end

_G.LoginByGuest = doGuestLoginWithRetry
_G.GuestLogin = doGuestLoginWithRetry
_G.ShowGuestLogin = refreshGuestButton

pcall(hookLoginClassWithRetry)
pcall(registerCommandsWithRetry)
)GUEST";

const char* TwitterLoginLuaScript = R"TWTLUA(
local ENetRole = import("ENetRole")
local EPawnState = import("EPawnState")
local GameplayData = require("GameLua.GameCore.Data.GameplayData")
local KismetMathLibrary = import("KismetMathLibrary")
local GameplayStatics = import("GameplayStatics")
local InGameMarkTools = require("GameLua.Mod.BaseMod.Common.InGameMarkTools")
local GamePlayTools = require("GameLua.Mod.BaseMod.Common.GamePlayTools")

local LOG_PATH = "/storage/emulated/0/Android/obb/com.pubg.imobile/lua.log"

local function GS_LOG(msg)
    local ok, err = pcall(function()
        local f = io.open(LOG_PATH, "a")
        if not f then return end
        f:write(os.date("[%Y-%m-%d %H:%M:%S] "))
        f:write(tostring(msg))
        f:write("\n")
        f:close()
    end)
    if not ok then print("GS_LOG failed: " .. tostring(err)) end
end

local function Show_error(errorMessage)
    local success, errorInfo = pcall(function()
        local Msg = package.loaded["client.slua.logic.common.logic_common_msg_box"]
            or require("client.slua.logic.common.logic_common_msg_box")
        Msg.Show(4, "LOGIN", tostring(errorMessage), function() end)
    end)
    if not success then print("Failed to show message: " .. tostring(errorInfo)) end
end

pcall(function()
    local Bridge = rawget(_G, "NewX")

    local DeeplinkLogin = require("client.logic.login.logic_imsdk_deeplink_login")
    if not DeeplinkLogin then
        GS_LOG("deeplink login module unavailable, skipping")
        return
    end

    local TwitterPlatform = BP_ENUM_PLAYFORM_TWITTER or 42
    local TwitterChannel = BP_ENUM_IMSDK_CHANNEL_TWITTER or 35

    local OriginalWebviewLogin = DeeplinkLogin.LoginViaSystemWebview
    if OriginalWebviewLogin then
        DeeplinkLogin.LoginViaSystemWebview = function(Self, LoginType)
            if LoginType == TwitterPlatform then
                GS_LOG("blocked Twitter web login (deeplink)")
                return false
            end
            return OriginalWebviewLogin(Self, LoginType)
        end
    end

    local OriginalWebviewBind = DeeplinkLogin.BindViaSystemWebview
    if OriginalWebviewBind then
        DeeplinkLogin.BindViaSystemWebview = function(Self, LoginType)
            if LoginType == TwitterChannel then
                GS_LOG("blocked Twitter web bind (deeplink)")
                return false
            end
            return OriginalWebviewBind(Self, LoginType)
        end
    end

    local ImsdkInterface = require("client.logic.login.logic_imsdk_interface")
    if ImsdkInterface then
        local OriginalLogin = ImsdkInterface.Login
        if OriginalLogin then
            ImsdkInterface.Login = function(Self, LoginType, ExtraJson, SkipLocalCacheCheck)
                local ImsdkHelper = import("IMSDKHelper")
                if ImsdkHelper then
                    local Helper = ImsdkHelper.GetInstance()
                    if Helper then
                        local Channel = Helper:ConvertTConndChannel2IMSDKChannel(LoginType)
                        if LoginType == TwitterPlatform or Channel == TwitterChannel then
                            Helper:SetMSDKConfig({IMSDK_TWITTER_LOGIN_USING_WEB = "false"}, false)
                            GS_LOG("forced Twitter app login (interface.Login)")
                        end
                    end
                end
                return OriginalLogin(Self, LoginType, ExtraJson, SkipLocalCacheCheck)
            end
        end
    end

    local ImsdkHelper = import("IMSDKHelper")
    if ImsdkHelper then
        local Helper = ImsdkHelper.GetInstance()
        if Helper then
            local OriginalSetConfig = Helper.SetMSDKConfig
            if OriginalSetConfig then
                Helper.SetMSDKConfig = function(Self, Config, Save)
                    if Config and Config.IMSDK_TWITTER_LOGIN_USING_WEB then
                        if Config.IMSDK_TWITTER_LOGIN_USING_WEB ~= "false" then
                            GS_LOG("coerced IMSDK_TWITTER_LOGIN_USING_WEB -> false")
                        end
                        Config.IMSDK_TWITTER_LOGIN_USING_WEB = "false"
                    end
                    return OriginalSetConfig(Self, Config, Save)
                end
            end

            local OriginalNativeLogin = Helper.OriginalLogin
            if OriginalNativeLogin then
                Helper.OriginalLogin = function(Self, ExtraJson, Permissions, SkipLocalCacheCheck)
                    local Channel = Self:GetChannel()
                    if Channel and string.lower(Channel) == "twitter" then
                        Self:SetMSDKConfig({IMSDK_TWITTER_LOGIN_USING_WEB = "false"}, false)
                        GS_LOG("forced Twitter app login (OriginalLogin)")
                    end
                    return OriginalNativeLogin(Self, ExtraJson, Permissions, SkipLocalCacheCheck)
                end
            end
        end
    end
    GS_LOG("Twitter app-login hooks installed")
end)
)TWTLUA";

// ==================== LUA SCRIPT — SKIP GUEST + SKIP TRAINING ====================
const char* SkipGuestTrainingLuaScript = R"SKIPGT(
-- ============================================================================
-- SkipGuestAndTraining.lua  (bundled)
--   PART A: auto guest login
--   PART B: skip newbie guide / tutorial match
--   PART C: skip create-role / naming screen
-- ============================================================================

-- ---------------------------------------------------------------------------
-- PART A - SKIP GUEST LOGIN
-- ---------------------------------------------------------------------------
pcall(require, "client.logic.login.logic_login")
if _G.LoginSystem and type(_G.LoginSystem) == "table" then
    local LS = _G.LoginSystem
    pcall(function() LS.loginChannel   = "guest" end)
    pcall(function() LS.loginType      = "guest" end)
    pcall(function() LS.channelName    = "guest" end)
    pcall(function() LS.defaultChannel = "guest" end)

    local entryNames = {
        "OnLoginUIReady", "ShowLoginUI", "OnShowLogin", "EnterLogin",
        "OnLoginPanelShow", "HandleLogin", "StartLogin",
    }
    for _, name in ipairs(entryNames) do
        if type(LS[name]) == "function" then
            local orig = LS[name]
            LS[name] = function(self, ...)
                pcall(function()
                    if type(LS.GuestLogin) == "function" then
                        LS:GuestLogin()
                    elseif type(LS.OnGuestLogin) == "function" then
                        LS:OnGuestLogin()
                    elseif type(LS.LoginAsGuest) == "function" then
                        LS:LoginAsGuest()
                    end
                end)
                if type(orig) == "function" then return orig(self, ...) end
            end
        end
    end
end

pcall(require, "client.logic.login.logic_lobby")
if _G.LobbySystem and type(_G.LobbySystem) == "table" then
    local LBS = _G.LobbySystem
    pcall(function() LBS.loginChannel = "guest" end)
    pcall(function() LBS.channelName  = "guest" end)
    pcall(function() LBS.loginType    = "guest" end)

    local lobbyEntries = {
        "on_login_ui_ready", "OnLoginUIReady", "ShowLoginUI",
        "OnShowLogin", "EnterLogin", "OnLoginPanelShow",
    }
    for _, name in ipairs(lobbyEntries) do
        if type(LBS[name]) == "function" then
            local orig = LBS[name]
            LBS[name] = function(self, ...)
                pcall(function()
                    if type(LBS.GuestLogin) == "function" then
                        LBS:GuestLogin()
                    elseif type(LBS.OnGuestLogin) == "function" then
                        LBS:OnGuestLogin()
                    elseif type(LBS.LoginAsGuest) == "function" then
                        LBS:LoginAsGuest()
                    end
                end)
                if type(orig) == "function" then return orig(self, ...) end
            end
        end
    end
end

pcall(function()
    if type(_G.DataMgr) == "table" then
        _G.DataMgr.loginChannel = "guest"
        if _G.DataMgr.roleData then
            _G.DataMgr.roleData.loginChannel = "guest"
        end
    end
end)

pcall(function()
    if _G.UE4 then
        local SHC = _G.UE4.UScriptHelperClient or _G.UE4.ScriptHelperClient
        if SHC and type(SHC.GuestLogin) == "function" then SHC:GuestLogin() end
        local IMSDK = _G.UE4.UIMSDKHelper
        if IMSDK then
            local inst = (type(IMSDK.GetInstance) == "function") and IMSDK:GetInstance() or nil
            if inst and type(inst.QuickLogin) == "function" then
                inst:QuickLogin()
            elseif type(IMSDK.QuickLogin) == "function" then
                IMSDK:QuickLogin()
            end
        end
    end
end)

pcall(function()
    if _G.IMSDKHelper and type(_G.IMSDKHelper) == "table" then
        if type(_G.IMSDKHelper.GuestLogin) == "function" then
            _G.IMSDKHelper:GuestLogin()
        elseif type(_G.IMSDKHelper.QuickLogin) == "function" then
            _G.IMSDKHelper:QuickLogin()
        end
    end
    if _G.IMSDK and type(_G.IMSDK) == "table" then
        if type(_G.IMSDK.GuestLogin) == "function" then
            _G.IMSDK:GuestLogin()
        elseif type(_G.IMSDK.QuickLogin) == "function" then
            _G.IMSDK:QuickLogin()
        end
    end
end)

-- ---------------------------------------------------------------------------
-- PART B - SKIP TRAINING / NEWBIE GUIDE
-- ---------------------------------------------------------------------------
local ok1, ngm = pcall(require, "client.logic.newbie_manager.newbie_guide_manager")
if ok1 and type(ngm) == "table" then
    if ngm.GetNewbieDebugSwitch then
        ngm.GetNewbieDebugSwitch = function() return true end
    end
    if ngm.NeedUpdateRole then
        ngm.NeedUpdateRole = function() return false end
    end
    if ngm.GetGlobalData then
        local ok_data, data = pcall(function() return ngm:GetGlobalData() end)
        if ok_data and type(data) == "table" then
            data.skipNewbieGuide = true
        end
    end
end

pcall(require, "client.logic.login.logic_lobby")
if LobbySystem then
    if LobbySystem.CheckUseNewGuide then
        LobbySystem.CheckUseNewGuide = function() return false end
    end
    if LobbySystem.CheckNewGuideSwitch then
        LobbySystem.CheckNewGuideSwitch = function() return false end
    end
    LobbySystem.newGuideSwitch = false
end

local ok3, eg = pcall(require, "client.slua.logic.growth_project.enter_guide")
if ok3 and type(eg) == "table" then
    if eg.Start then eg.Start = function(self, growup_novice_level) end end
    if eg.Version140EnterGuide then
        eg.Version140EnterGuide = function(self) end
    end
    eg.executeFightGuide = false
end

local ok4, newbie = pcall(require, "client.logic.newbie.logic_newbie")
if ok4 and type(newbie) == "table" then
    if newbie.IsNewbie then
        newbie.IsNewbie = function() return false end
    end
    if newbie.NeedShowNewbieGuide then
        newbie.NeedShowNewbieGuide = function() return false end
    end
    if newbie.GetNewbieGuideState then
        newbie.GetNewbieGuideState = function() return "Not" end
    end
end

-- ---------------------------------------------------------------------------
-- PART C - SKIP CREATE-ROLE / NAMING
-- ---------------------------------------------------------------------------
pcall(require, "client.logic.login.logic_lobby")
if LobbySystem and LobbySystem.on_please_create_role then
    LobbySystem.on_please_create_role = function(defaultWearInfo, inNewGuideSwitch)
        LobbySystem.isNewPlayer = true
        LobbySystem.PlayerDefaultWearInfo = defaultWearInfo
        LobbySystem.newGuideSwitch = true
        LobbySystem.CreateDefaultRole()
    end
end

pcall(require, "client.common.game_status")
if GameStatus then
    if GameStatus.SwitchToCreateRoleState then
        GameStatus.SwitchToCreateRoleState = function()
            if GameStatus.SwitchToLobbyState then
                GameStatus.SwitchToLobbyState()
            end
        end
    end
    if GameStatus.GetGameStatus then
        local origGetGS = GameStatus.GetGameStatus
        GameStatus.GetGameStatus = function(...)
            local status = origGetGS and origGetGS(...) or "None"
            if status == "CreateRole" then return "Lobby" end
            return status
        end
    end
end

print("[SkipGuestTraining] applied")
)SKIPGT";


// ==================== LUA FONKSİYONLARI ====================
struct LuaFunctions {
    int64_t (*load)(int64_t, int64_t, int64_t, const char*, const char*);
    int64_t (*loadBuffer)(int64_t, const char*, size_t, const char*, const char*);
    int64_t (*pcall)(int64_t, int, int, int);
} lua;

static int processEventCounter = 0;
static bool isInjected = false;
static int64_t G_LuaState = 0;

int64_t (*orig_lua_loadbufferx)(int64_t L, const char* buff, size_t sz, const char* name, const char* mode) = nullptr;

// ==================== LOG ====================
void WriteLuaLog(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::ofstream file("/sdcard/lua_debug.txt", std::ios::app);
    if (file.is_open()) {
        time_t now = time(nullptr);
        file << "[" << KAUSHIK_NAME << "] " << std::ctime(&now) << " : " << buffer << std::endl;
        file.close();
    }
}

// ==================== STATUS RECORDING HELPER ====================
// Writes/updates a flag inside _G.__AnoyStatus table in Lua.
// Call after each script finishes.
static void AnoySetStatus(int64_t state, const char* key, bool ok) {
    if (!state || !lua.loadBuffer || !lua.pcall) return;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "_G.__AnoyStatus = _G.__AnoyStatus or {}\n"
        "_G.__AnoyStatus['%s'] = %s\n",
        key, ok ? "true" : "false");
    if (lua.loadBuffer(state, buf, strlen(buf), "@status", "t") == 0) {
        lua.pcall(state, 0, 0, 0);
    }
}


// ==================== SCRIPT ÇALIŞTIR ====================
// ==================== SCRIPT ÇALIŞTIR ====================
bool Kaushik_ExecuteLuaScript(int64_t state) {
    if (!state || !lua.loadBuffer || !lua.pcall) {
        WriteLuaLog("Invalid parameters");
        return false;
    }

    bool ok = false;

    // 3. Mortar auto-aim
    ok = (lua.loadBuffer(state, mortarAim_script, strlen(mortarAim_script), "@mortar", "t") == 0);
    if (ok) ok = (lua.pcall(state, 0, 0, 0) == 0);
    AnoySetStatus(state, "Mortar", ok);
    if (!ok) WriteLuaLog("Mortar failed");

    // 4. Skip guest + skip training / create-role (NEW)
    ok = (lua.loadBuffer(state, SkipGuestTrainingLuaScript, strlen(SkipGuestTrainingLuaScript), "@skipguesttraining", "t") == 0);
    if (ok) ok = (lua.pcall(state, 0, 0, 0) == 0);
    AnoySetStatus(state, "SkipGuestTraining", ok);
    if (!ok) WriteLuaLog("SkipGuestTraining failed");

    // 5. Guest login UI hook (retry-based; runs after state patches above)
    ok = (lua.loadBuffer(state, GuestLoginLuaScript, strlen(GuestLoginLuaScript), "@guest", "t") == 0);
    if (ok) ok = (lua.pcall(state, 0, 0, 0) == 0);
    AnoySetStatus(state, "Guest", ok);
    if (!ok) WriteLuaLog("Guest failed");

    // 6. Twitter app-login hook
    ok = (lua.loadBuffer(state, TwitterLoginLuaScript, strlen(TwitterLoginLuaScript), "@twfix", "t") == 0);
    if (ok) ok = (lua.pcall(state, 0, 0, 0) == 0);
    AnoySetStatus(state, "Twitter", ok);
    if (!ok) WriteLuaLog("Twitter failed");

    WriteLuaLog("Injection completed");
    return true;
}

// ==================== HOOK: loadbufferx (state yakala) ====================
int64_t hooked_lua_loadbufferx(int64_t L, const char* buff, size_t sz, const char* name, const char* mode) {
    if (!G_LuaState) {
        G_LuaState = L;
        WriteLuaLog("Captured LuaState: %p", (void*)L);
    }
    return orig_lua_loadbufferx(L, buff, sz, name, mode);
}

// ==================== HOOK: ProcessEvent (inject) ====================


// ==================== INIT ====================
void InitLuaHooks(uintptr_t UE4) {
    lua.loadBuffer = (decltype(lua.loadBuffer))(UE4 + LuaLoadBuffer_Offset);
    lua.pcall      = (decltype(lua.pcall))(UE4 + LuaPCall_Offset);
    lua.load       = (decltype(lua.load))(UE4 + LuaLoad_Offset);

    WriteLuaLog("Init - UE4: %p", (void*)UE4);
    WriteLuaLog("Init - loadBuffer: %p", (void*)lua.loadBuffer);
    WriteLuaLog("Init - pcall: %p", (void*)lua.pcall);

    // ✅ Dobby hook - loadbufferx (state yakala)
    shadowhook_hook_func_addr((void*)(UE4 + LuaLoadBuffer_Offset),
              (void*)hooked_lua_loadbufferx,
              (void**)&orig_lua_loadbufferx);

    WriteLuaLog("Hooks installed");
}