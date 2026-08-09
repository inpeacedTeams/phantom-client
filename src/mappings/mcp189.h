#pragma once

// MCP 1.8.9 mappings
// For each field: MCP name, SRG name (field_XXXXX), Notch name
// Lunar may use any of these depending on build.
// Try SRG first, then MCP, then Notch.

namespace MCP {

// ========== Minecraft (net.minecraft.client.Minecraft) ==========
// Notch: ave
namespace Minecraft {
    constexpr const char* className_notch  = "ave";
    constexpr const char* className_mcp    = "net/minecraft/client/Minecraft";

    // Fields
    constexpr const char* thePlayer_mcp    = "thePlayer";
    constexpr const char* thePlayer_srg    = "field_71439_g";
    constexpr const char* theWorld_mcp     = "theWorld";
    constexpr const char* theWorld_srg     = "field_71441_e";
    constexpr const char* gameSettings_mcp = "gameSettings";
    constexpr const char* gameSettings_srg = "field_71474_y";
    constexpr const char* objectMouseOver_srg = "field_71476_x";
    constexpr const char* timer_srg        = "field_71428_T";
    constexpr const char* playerController_srg = "field_71442_b";
    constexpr const char* currentScreen_srg = "field_71462_r";
    constexpr const char* rightClickDelayTimer_srg = "field_71467_ac";
    constexpr const char* renderManager_srg = "field_175616_W";

    // Methods
    constexpr const char* getMinecraft_srg = "func_71410_x";
    constexpr const char* getMinecraft_sig = "()Ljava/lang/Object;";
}

// ========== Entity (net.minecraft.entity.Entity) ==========
// Notch: pk
namespace Entity {
    constexpr const char* className_notch = "pk";

    constexpr const char* posX_srg        = "field_70165_t";
    constexpr const char* posY_srg        = "field_70163_u";
    constexpr const char* posZ_srg        = "field_70161_v";
    constexpr const char* prevPosX_srg    = "field_70169_q";
    constexpr const char* prevPosY_srg    = "field_70167_r";
    constexpr const char* prevPosZ_srg    = "field_70166_s";
    constexpr const char* motionX_srg     = "field_70159_w";
    constexpr const char* motionY_srg     = "field_70181_x";
    constexpr const char* motionZ_srg     = "field_70179_y";
    constexpr const char* rotationYaw_srg = "field_70177_z";
    constexpr const char* rotationPitch_srg = "field_70125_A";
    constexpr const char* onGround_srg    = "field_70122_E";
    constexpr const char* isDead_srg      = "field_70128_L";
    constexpr const char* hurtResistantTime_srg = "field_70172_ad";
    constexpr const char* ticksExisted_srg = "field_70173_aa";
    constexpr const char* width_srg       = "field_70130_N";
    constexpr const char* height_srg      = "field_70131_O";

    // MCP names (fallback)
    constexpr const char* posX_mcp = "posX";
    constexpr const char* posY_mcp = "posY";
    constexpr const char* posZ_mcp = "posZ";
    constexpr const char* motionX_mcp = "motionX";
    constexpr const char* motionY_mcp = "motionY";
    constexpr const char* motionZ_mcp = "motionZ";
    constexpr const char* rotationYaw_mcp = "rotationYaw";
    constexpr const char* rotationPitch_mcp = "rotationPitch";
    constexpr const char* onGround_mcp = "onGround";

    // Methods
    constexpr const char* getDistanceToEntity_srg = "func_70032_d";
    constexpr const char* getName_srg     = "func_70005_c_";
    constexpr const char* getBoundingBox_srg = "func_174813_aQ";
}

// ========== EntityLivingBase ==========
// Notch: pr
namespace EntityLivingBase {
    constexpr const char* className_notch = "pr";

    constexpr const char* health_srg      = "field_70725_aQ";
    constexpr const char* maxHealth_method_srg = "func_110138_aP";
    constexpr const char* swingProgress_srg = "field_70733_aJ";
    constexpr const char* isSwingInProgress_srg = "field_82175_bq";
    constexpr const char* hurtTime_srg    = "field_70737_aN";
    constexpr const char* moveForward_srg = "field_70701_bs";
    constexpr const char* moveStrafing_srg = "field_70702_br";
    constexpr const char* jumpMovementFactor_srg = "field_70747_aH";

    constexpr const char* health_mcp      = "health";
    constexpr const char* hurtTime_mcp    = "hurtTime";
}

// ========== EntityPlayerSP ==========
// Notch: bew
namespace EntityPlayerSP {
    constexpr const char* className_notch = "bew";
    constexpr const char* sendQueue_srg   = "field_71174_a";
    constexpr const char* sendQueue_mcp   = "sendQueue";
    constexpr const char* sprintToggleTimer_srg = "field_71156_d";
    constexpr const char* serverSprintState_srg = "field_175171_bO";
}

// ========== GameSettings ==========
namespace GameSettings {
    constexpr const char* gammaSetting_srg = "field_74333_Y";
    constexpr const char* gammaSetting_mcp = "gammaSetting";
    constexpr const char* fovSetting_srg  = "field_74334_X";
    constexpr const char* sensitivity_srg = "field_74341_c";
    constexpr const char* thirdPersonView_srg = "field_74320_O";
    constexpr const char* keyBindSprint_srg = "field_151444_V";
}

// ========== Timer ==========
namespace Timer {
    constexpr const char* renderPartialTicks_srg = "field_74281_c";
    constexpr const char* timerSpeed_srg  = "field_74278_d";
    constexpr const char* renderPartialTicks_mcp = "renderPartialTicks";
    constexpr const char* timerSpeed_mcp  = "timerSpeed";
}

// ========== World ==========
namespace World {
    constexpr const char* playerEntities_srg = "field_73010_i";
    constexpr const char* loadedEntityList_srg = "field_72996_f";
    constexpr const char* playerEntities_mcp = "playerEntities";
}

// ========== PlayerControllerMP ==========
namespace PlayerControllerMP {
    constexpr const char* curBlockDamageMP_srg = "field_78770_f";
    constexpr const char* blockHitDelay_srg = "field_78781_i";
}

// ========== RenderManager ==========
namespace RenderManager {
    constexpr const char* renderPosX_srg  = "field_78725_b";
    constexpr const char* renderPosY_srg  = "field_78726_c";
    constexpr const char* renderPosZ_srg  = "field_78723_d";
}

// ========== AxisAlignedBB ==========
namespace AxisAlignedBB {
    constexpr const char* minX = "minX";
    constexpr const char* minY = "minY";
    constexpr const char* minZ = "minZ";
    constexpr const char* maxX = "maxX";
    constexpr const char* maxY = "maxY";
    constexpr const char* maxZ = "maxZ";
    constexpr const char* minX_srg = "field_72340_a";
    constexpr const char* minY_srg = "field_72338_b";
    constexpr const char* minZ_srg = "field_72339_c";
    constexpr const char* maxX_srg = "field_72336_d";
    constexpr const char* maxY_srg = "field_72337_e";
    constexpr const char* maxZ_srg = "field_72334_f";
}

} // namespace MCP
