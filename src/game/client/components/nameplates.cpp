#include "nameplates.h"

#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol7.h>
#include <engine/textrender.h>

#include <generated/client_data.h>

#include <game/client/QmUi/QmAnim.h>
#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
//枚举
enum class EHookStrongWeakState
{
	WEAK,
	NEUTRAL,
	STRONG
};

enum class ENameplateCoreRow
{
	NAME,
	CLAN,
	HOOK,
	COORDS,
	KEYS,
	NUM_ROWS
};

static constexpr size_t kNameplateCoreRowCount = static_cast<size_t>(ENameplateCoreRow::NUM_ROWS);
static constexpr std::array<ENameplateCoreRow, kNameplateCoreRowCount> s_aDefaultNameplateCoreRowsTopToBottom = {
	ENameplateCoreRow::KEYS,
	ENameplateCoreRow::COORDS,
	ENameplateCoreRow::HOOK,
	ENameplateCoreRow::CLAN,
	ENameplateCoreRow::NAME};

static bool FocusModeHidesChat()
{
	return g_Config.m_QmFocusMode != 0 && g_Config.m_QmFocusModeHideChat != 0;
}

struct SChatBubbleAnimState
{
	bool m_Initialized = false;
	bool m_LastVisible = false;
	bool m_LastTyping = false;
	int64_t m_LastChatBubbleStartTick = 0;
	float m_LastTargetAlpha = std::numeric_limits<float>::quiet_NaN();
	float m_LastTargetScale = std::numeric_limits<float>::quiet_NaN();
	float m_LastTargetSlide = std::numeric_limits<float>::quiet_NaN();
	float m_LastTargetFillChars = std::numeric_limits<float>::quiet_NaN();
	STextContainerIndex m_TextContainerIndex;
	float m_CachedTextWidth = 0.0f;
	float m_CachedTextHeight = 0.0f;
	float m_CachedFontSize = -INFINITY;
	float m_CachedLineWidth = -INFINITY;
	char m_aCachedText[256] = "";
	char m_aLayoutText[256] = "";
};

struct SCoordXAlignState
{
	bool m_Active = false;
	bool m_Aligned = false;
	float m_WindowStartTime = 0.0f;
};

struct SCoordXAlignFrameState
{
	bool m_LocalAligned = false;
	int m_LocalClientId = -1;
	int m_LocalRoundedX = 0;
};

static uint64_t ChatBubbleAnimNodeKey(int ClientId)
{
	static const uint64_t s_BaseKey = static_cast<uint64_t>(str_quickhash("chat_bubble_anim_v2"));
	return (s_BaseKey << 32) | static_cast<uint64_t>(ClientId & 0xffff);
}

static float ResolveChatBubbleAnimValue(
	CUiV2AnimationRuntime &AnimRuntime,
	uint64_t NodeKey,
	EUiAnimProperty Property,
	float Target,
	float DurationSec,
	EEasing Easing,
	float &LastTarget,
	bool ForceRequest = false)
{
	constexpr float Epsilon = 0.001f;
	const float Current = AnimRuntime.GetValue(NodeKey, Property, Target);
	const bool TargetChanged = !std::isfinite(LastTarget) || std::abs(Target - LastTarget) > Epsilon;
	const bool NeedsSync = !AnimRuntime.HasActiveAnimation(NodeKey, Property) && std::abs(Target - Current) > Epsilon;
	if(ForceRequest || TargetChanged || NeedsSync)
	{
		SUiAnimRequest Request;
		Request.m_NodeKey = NodeKey;
		Request.m_Property = Property;
		Request.m_Target = Target;
		Request.m_Transition.m_DurationSec = DurationSec;
		Request.m_Transition.m_DelaySec = 0.0f;
		Request.m_Transition.m_Priority = 1;
		Request.m_Transition.m_Interrupt = EUiAnimInterruptPolicy::MERGE_TARGET;
		Request.m_Transition.m_Easing = Easing;
		AnimRuntime.RequestAnimation(Request);
		LastTarget = Target;
	}
	return AnimRuntime.GetValue(NodeKey, Property, Target);
}

static constexpr int NAMEPLATE_FREE_MOVE_OFFSET_MIN = -300;
static constexpr int NAMEPLATE_FREE_MOVE_OFFSET_MAX = 300;
// Multiplier applied to the baseline frame to give users more room to move
// nameplate elements around. Width and height are both scaled by this factor
// around the baseline frame's center.
static constexpr float NAMEPLATE_FREE_MOVE_FRAME_SCALE = 3.0f;
static constexpr float NAMEPLATE_FREE_MOVE_SNAP_DISTANCE = 8.0f;

static void ScaleFrameAroundCenter(vec2 &Min, vec2 &Max, float Scale)
{
	const vec2 Center = (Min + Max) * 0.5f;
	const vec2 HalfExtent = (Max - Min) * 0.5f * Scale;
	Min = Center - HalfExtent;
	Max = Center + HalfExtent;
}

static bool NameplateFreeMoveEnabled()
{
	return g_Config.m_QmNameplateFreeMove != 0 || g_Config.m_QmNameplateFreeMoveX != 0 || g_Config.m_QmNameplateFreeMoveY != 0;
}

static bool NameplatePointInFrame(vec2 Position, vec2 FrameMin, vec2 FrameMax)
{
	return Position.x >= FrameMin.x &&
	       Position.x <= FrameMax.x &&
	       Position.y >= FrameMin.y &&
	       Position.y <= FrameMax.y;
}

static int NameplateSnapCoreRowOffsetX(int OffsetX, int LimitMinX, int LimitMaxX, vec2 RowCenter, vec2 RowSize, vec2 FrameMin, vec2 FrameMax)
{
	const float RowMinX = RowCenter.x - RowSize.x / 2.0f;
	const float RowMaxX = RowCenter.x + RowSize.x / 2.0f;
	const float FrameCenterX = (FrameMin.x + FrameMax.x) / 2.0f;
	const int aSnapTargets[] = {
		std::clamp(round_to_int(FrameMin.x - RowMinX), LimitMinX, LimitMaxX),
		std::clamp(round_to_int(FrameCenterX - RowCenter.x), LimitMinX, LimitMaxX),
		std::clamp(round_to_int(FrameMax.x - RowMaxX), LimitMinX, LimitMaxX),
	};

	int SnappedOffsetX = OffsetX;
	int BestDistance = round_to_int(NAMEPLATE_FREE_MOVE_SNAP_DISTANCE) + 1;
	for(const int TargetOffsetX : aSnapTargets)
	{
		const int Distance = std::abs(OffsetX - TargetOffsetX);
		if(Distance <= NAMEPLATE_FREE_MOVE_SNAP_DISTANCE && Distance < BestDistance)
		{
			SnappedOffsetX = TargetOffsetX;
			BestDistance = Distance;
		}
	}
	return SnappedOffsetX;
}

static int *NameplateCoreRowOffsetX(ENameplateCoreRow Row)
{
	switch(Row)
	{
	case ENameplateCoreRow::NAME:
		return &g_Config.m_QmNameplateNameOffsetX;
	case ENameplateCoreRow::CLAN:
		return &g_Config.m_QmNameplateClanOffsetX;
	case ENameplateCoreRow::HOOK:
		return &g_Config.m_QmNameplateHookOffsetX;
	case ENameplateCoreRow::COORDS:
		return &g_Config.m_QmNameplateCoordsOffsetX;
	case ENameplateCoreRow::KEYS:
		return &g_Config.m_QmNameplateKeysOffsetX;
	case ENameplateCoreRow::NUM_ROWS:
		break;
	}
	return nullptr;
}

static int *NameplateCoreRowOffsetY(ENameplateCoreRow Row)
{
	switch(Row)
	{
	case ENameplateCoreRow::NAME:
		return &g_Config.m_QmNameplateNameOffsetY;
	case ENameplateCoreRow::CLAN:
		return &g_Config.m_QmNameplateClanOffsetY;
	case ENameplateCoreRow::HOOK:
		return &g_Config.m_QmNameplateHookOffsetY;
	case ENameplateCoreRow::COORDS:
		return &g_Config.m_QmNameplateCoordsOffsetY;
	case ENameplateCoreRow::KEYS:
		return &g_Config.m_QmNameplateKeysOffsetY;
	case ENameplateCoreRow::NUM_ROWS:
		break;
	}
	return nullptr;
}

static vec2 NameplateCoreRowOffset(ENameplateCoreRow Row)
{
	if(!NameplateFreeMoveEnabled())
		return vec2(0.0f, 0.0f);
	int *pOffsetX = NameplateCoreRowOffsetX(Row);
	int *pOffsetY = NameplateCoreRowOffsetY(Row);
	const float OffsetX = pOffsetX != nullptr ? (float)*pOffsetX : 0.0f;
	const float OffsetY = pOffsetY != nullptr ? (float)*pOffsetY : 0.0f;
	return vec2(OffsetX, OffsetY);
}

struct SNameplateCoreRowRect
{
	ENameplateCoreRow m_Row = ENameplateCoreRow::NUM_ROWS;
	vec2 m_Min = vec2(0.0f, 0.0f);
	vec2 m_Max = vec2(0.0f, 0.0f);
	bool m_Visible = false;
};

class CNamePlateData
{
public:
	bool m_Local; // TClient
	bool m_InGame;
	ColorRGBA m_Color;
	bool m_ShowName;
	char m_aName[std::max<size_t>(MAX_NAME_LENGTH, protocol7::MAX_NAME_ARRAY_SIZE)];
	bool m_ShowFriendMark;
	bool m_ShowClientId;
	int m_ClientId;
	float m_FontSizeClientId;
	bool m_ClientIdSeparateLine;
	float m_FontSize;
	bool m_ShowClan;
	char m_aClan[std::max<size_t>(MAX_CLAN_LENGTH, protocol7::MAX_CLAN_ARRAY_SIZE)];
	float m_FontSizeClan;
	bool m_ShowCoordX;
	bool m_ShowCoordY;
	bool m_ShowCoords;
	bool m_CoordXAlignHint;
	bool m_CoordXAlignHintStrict;
	bool m_CoordXAligned;
	ColorRGBA m_CoordXAlignColor;
	vec2 m_Coords;
	float m_FontSizeCoords;
	bool m_ShowDirection;
	bool m_DirLeft;
	bool m_DirJump;
	bool m_DirRight;
	float m_FontSizeDirection;
	bool m_ShowHookStrongWeak;
	EHookStrongWeakState m_HookStrongWeakState;
	bool m_ShowHookStrongWeakId;
	int m_HookStrongWeakId;
	float m_FontSizeHookStrongWeak;
};

// Part Types

static constexpr float DEFAULT_PADDING = 5.0f;

class CNamePlatePart
{
protected:
	vec2 m_Size = vec2(0.0f, 0.0f);
	vec2 m_Padding = vec2(DEFAULT_PADDING, DEFAULT_PADDING);
	bool m_NewLine = false; // Whether this part is a new line (doesn't do anything else)
	bool m_Visible = true; // Whether this part is visible
	bool m_ShiftOnInvis = false; // Whether when not visible will still take up space
	CNamePlatePart(CGameClient &This) {}

public:
	virtual void Update(CGameClient &This, const CNamePlateData &Data) {}
	virtual void Reset(CGameClient &This) {}
	virtual void Render(CGameClient &This, vec2 Pos) const {}
	vec2 Size() const { return m_Size; }
	vec2 Padding() const { return m_Padding; }
	bool NewLine() const { return m_NewLine; }
	bool Visible() const { return m_Visible; }
	bool ShiftOnInvis() const { return m_ShiftOnInvis; }
	CNamePlatePart() = delete;
	virtual ~CNamePlatePart() = default;
};

using PartsVector = std::vector<std::unique_ptr<CNamePlatePart>>;

static constexpr ColorRGBA s_OutlineColor = ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f);

class CNamePlatePartText : public CNamePlatePart
{
protected:
	STextContainerIndex m_TextContainerIndex;
	virtual bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) = 0;
	virtual void UpdateText(CGameClient &This, const CNamePlateData &Data) = 0;
	ColorRGBA m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	CNamePlatePartText(CGameClient &This) :
		CNamePlatePart(This)
	{
		Reset(This);
	}

public:
	void Update(CGameClient &This, const CNamePlateData &Data) override
	{
		if(!UpdateNeeded(This, Data) && m_TextContainerIndex.Valid())
			return;

		// Set flags
		unsigned int Flags = ETextRenderFlags::TEXT_RENDER_FLAG_NO_FIRST_CHARACTER_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_LAST_CHARACTER_ADVANCE;
		if(Data.m_InGame)
			Flags |= ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT; // Prevent jittering from rounding
		This.TextRender()->SetRenderFlags(Flags);

		if(Data.m_InGame)
		{
			// Create text at standard zoom
			float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
			This.Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);
			This.Graphics()->MapScreenToInterface(This.m_Camera.m_Center.x, This.m_Camera.m_Center.y);
			This.TextRender()->DeleteTextContainer(m_TextContainerIndex);
			UpdateText(This, Data);
			This.Graphics()->MapScreen(ScreenX0, ScreenY0, ScreenX1, ScreenY1);
		}
		else
		{
			UpdateText(This, Data);
		}

		This.TextRender()->SetRenderFlags(0);

		if(!m_TextContainerIndex.Valid())
		{
			m_Visible = false;
			return;
		}

		const STextBoundingBox Container = This.TextRender()->GetBoundingBoxTextContainer(m_TextContainerIndex);
		m_Size = vec2(Container.m_W, Container.m_H);
	}
	void Reset(CGameClient &This) override
	{
		This.TextRender()->DeleteTextContainer(m_TextContainerIndex);
	}
	void Render(CGameClient &This, vec2 Pos) const override
	{
		if(!m_TextContainerIndex.Valid())
			return;

		ColorRGBA OutlineColor, Color;
		Color = m_Color;
		OutlineColor = s_OutlineColor.WithMultipliedAlpha(m_Color.a);
		This.TextRender()->RenderTextContainer(m_TextContainerIndex,
			Color, OutlineColor,
			Pos.x - Size().x / 2.0f, Pos.y - Size().y / 2.0f);
	}
};

class CNamePlatePartIcon : public CNamePlatePart
{
protected:
	IGraphics::CTextureHandle m_Texture;
	float m_Rotation = 0.0f;
	ColorRGBA m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	CNamePlatePartIcon(CGameClient &This) :
		CNamePlatePart(This) {}

public:
	void Render(CGameClient &This, vec2 Pos) const override
	{
		IGraphics::CQuadItem QuadItem(Pos.x - Size().x / 2.0f, Pos.y - Size().y / 2.0f, Size().x, Size().y);
		This.Graphics()->TextureSet(m_Texture);
		This.Graphics()->QuadsBegin();
		This.Graphics()->SetColor(m_Color);
		This.Graphics()->QuadsSetRotation(m_Rotation);
		This.Graphics()->QuadsDrawTL(&QuadItem, 1);
		This.Graphics()->QuadsEnd();
		This.Graphics()->QuadsSetRotation(0.0f);
	}
};

class CNamePlatePartSprite : public CNamePlatePart
{
protected:
	IGraphics::CTextureHandle m_Texture;
	int m_Sprite = -1;
	int m_SpriteFlags = 0;
	float m_Rotation = 0.0f;
	ColorRGBA m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	CNamePlatePartSprite(CGameClient &This) :
		CNamePlatePart(This) {}

public:
	void Render(CGameClient &This, vec2 Pos) const override
	{
		This.Graphics()->TextureSet(m_Texture);
		This.Graphics()->QuadsSetRotation(m_Rotation);
		This.Graphics()->QuadsBegin();
		This.Graphics()->SetColor(m_Color);
		This.Graphics()->SelectSprite(m_Sprite, m_SpriteFlags);
		This.Graphics()->DrawSprite(Pos.x, Pos.y, Size().x, Size().y);
		This.Graphics()->QuadsEnd();
		This.Graphics()->QuadsSetRotation(0.0f);
	}
};

// Part Definitions

class CNamePlatePartNewLine : public CNamePlatePart
{
public:
	CNamePlatePartNewLine(CGameClient &This) :
		CNamePlatePart(This)
	{
		m_NewLine = true;
	}
};

enum Direction
{
	DIRECTION_LEFT,
	DIRECTION_UP,
	DIRECTION_RIGHT
};

class CNamePlatePartDirection : public CNamePlatePartIcon
{
private:
	int m_Direction;

public:
	CNamePlatePartDirection(CGameClient &This, Direction Dir) :
		CNamePlatePartIcon(This)
	{
		m_Texture = g_pData->m_aImages[IMAGE_ARROW].m_Id;
		m_Direction = Dir;
		switch(m_Direction)
		{
		case DIRECTION_LEFT:
			m_Rotation = pi;
			break;
		case DIRECTION_UP:
			m_Rotation = pi / -2.0f;
			break;
		case DIRECTION_RIGHT:
			m_Rotation = 0.0f;
			break;
		}
	}
	void Update(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Texture = g_pData->m_aImages[IMAGE_ARROW].m_Id;
		if(!Data.m_ShowDirection)
		{
			m_ShiftOnInvis = false;
			m_Visible = false;
			return;
		}
		m_ShiftOnInvis = true; // Only shift (horizontally) the other parts if directions as a whole is visible
		m_Size = vec2(Data.m_FontSizeDirection, Data.m_FontSizeDirection);
		m_Padding.y = m_Size.y / 2.0f;
		switch(m_Direction)
		{
		case DIRECTION_LEFT:
			m_Visible = Data.m_DirLeft;
			break;
		case DIRECTION_UP:
			m_Visible = Data.m_DirJump;
			break;
		case DIRECTION_RIGHT:
			m_Visible = Data.m_DirRight;
			break;
		}
		m_Color.a = Data.m_Color.a;
	}
};

class CNamePlatePartClientId : public CNamePlatePartText
{
private:
	int m_ClientId = -1;
	static_assert(MAX_CLIENTS <= 999, "Make this buffer bigger");
	char m_aText[5] = "";
	float m_FontSize = -INFINITY;
	bool m_ClientIdSeparateLine = false;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_ShowClientId && (Data.m_ClientIdSeparateLine == m_ClientIdSeparateLine);
		if(!m_Visible)
			return false;
		m_Color = Data.m_Color;
		return m_FontSize != Data.m_FontSizeClientId || m_ClientId != Data.m_ClientId;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeClientId;
		m_ClientId = Data.m_ClientId;
		if(m_ClientIdSeparateLine)
			str_format(m_aText, sizeof(m_aText), "%d", m_ClientId);
		else
			str_format(m_aText, sizeof(m_aText), "%d:", m_ClientId);
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartClientId(CGameClient &This, bool ClientIdSeparateLine) :
		CNamePlatePartText(This)
	{
		m_ClientIdSeparateLine = ClientIdSeparateLine;
	}
};

class CNamePlatePartFriendMark : public CNamePlatePartText
{
private:
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_ShowFriendMark;
		if(!m_Visible)
			return false;
		m_Color = ColorRGBA(1.0f, 0.0f, 0.0f, Data.m_Color.a);
		return m_FontSize != Data.m_FontSize;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSize;
		CTextCursor Cursor;
		This.TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, FontIcons::FONT_ICON_HEART);
		This.TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	}

public:
	CNamePlatePartFriendMark(CGameClient &This) :
		CNamePlatePartText(This)
	{
		m_Color = ColorRGBA(1.0f, 0.0f, 0.0f);
	}
};

class CNamePlatePartName : public CNamePlatePartText
{
private:
	char m_aText[std::max<size_t>(MAX_NAME_LENGTH, protocol7::MAX_NAME_ARRAY_SIZE)] = "";
	float m_FontSize = -INFINITY;
	bool m_IsLocal = false;
	float m_Alpha = 1.0f;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_ShowName;
		if(!m_Visible)
			return false;
		m_Color = Data.m_Color;
		m_IsLocal = Data.m_Local;
		if(!m_IsLocal)
		{
			if(Data.m_InGame)
			{
				for(const auto Id : This.m_aLocalIds)
				{
					if(Id == Data.m_ClientId)
					{
						m_IsLocal = true;
						break;
					}
				}
			}
			else
			{
				m_IsLocal = Data.m_ClientId == 0 || Data.m_ClientId == 1;
			}
		}
		m_Alpha = Data.m_Color.a;
		// TClient
		if(g_Config.m_TcWarList)
		{
			if(This.m_WarList.GetWarData(Data.m_ClientId).m_WarName)
				m_Color = This.m_WarList.GetNameplateColor(Data.m_ClientId).WithAlpha(Data.m_Color.a);
			else if(This.m_WarList.GetWarData(Data.m_ClientId).m_WarClan)
				m_Color = This.m_WarList.GetClanColor(Data.m_ClientId).WithAlpha(Data.m_Color.a);
		}
		return m_FontSize != Data.m_FontSize || str_comp(m_aText, Data.m_aName) != 0;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSize;
		str_copy(m_aText, Data.m_aName, sizeof(m_aText));
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

	void Render(CGameClient &This, vec2 Pos) const override
	{
		if(!m_TextContainerIndex.Valid())
			return;

		ColorRGBA OutlineColor, Color;
		Color = m_Color;

		// Rainbow name for local player (same style as QmClient sidebar)
		if(m_IsLocal && g_Config.m_QmRainbowName)
		{
			const float Time = This.Client()->GlobalTime();
			const float Hue = std::fmod(Time * 0.15f, 1.0f);
			ColorHSLA Hsla(Hue, 0.7f, 0.65f, m_Alpha);
			Color = color_cast<ColorRGBA>(Hsla);
		}

		OutlineColor = s_OutlineColor.WithMultipliedAlpha(Color.a);
		This.TextRender()->RenderTextContainer(m_TextContainerIndex,
			Color, OutlineColor,
			Pos.x - Size().x / 2.0f, Pos.y - Size().y / 2.0f);
	}

public:
	CNamePlatePartName(CGameClient &This) :
		CNamePlatePartText(This) {}
};

class CNamePlatePartClan : public CNamePlatePartText
{
private:
	char m_aText[std::max<size_t>(MAX_CLAN_LENGTH, protocol7::MAX_CLAN_ARRAY_SIZE)] = "";
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_ShowClan;
		if(!m_Visible)
			return false;
		m_Color = Data.m_Color;
		// TClient
		if(This.m_WarList.GetWarData(Data.m_ClientId).m_WarClan)
			m_Color = This.m_WarList.GetClanColor(Data.m_ClientId).WithAlpha(Data.m_Color.a);
		return m_FontSize != Data.m_FontSizeClan || str_comp(m_aText, Data.m_aClan[0] != '\0' ? Data.m_aClan : " ") != 0;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeClan;
		str_copy(m_aText, Data.m_aClan[0] != '\0' ? Data.m_aClan : " ", sizeof(m_aText));
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartClan(CGameClient &This) :
		CNamePlatePartText(This) {}
};

class CNamePlatePartHookStrongWeak : public CNamePlatePartSprite
{
protected:
	void Update(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Texture = g_pData->m_aImages[IMAGE_STRONGWEAK].m_Id;
		m_Visible = Data.m_ShowHookStrongWeak;
		if(!m_Visible)
			return;
		m_Size = vec2(Data.m_FontSizeHookStrongWeak + DEFAULT_PADDING, Data.m_FontSizeHookStrongWeak + DEFAULT_PADDING);
		switch(Data.m_HookStrongWeakState)
		{
		case EHookStrongWeakState::STRONG:
			m_Sprite = SPRITE_HOOK_STRONG;
			m_Color = color_cast<ColorRGBA>(ColorHSLA(6401973));
			break;
		case EHookStrongWeakState::NEUTRAL:
			m_Sprite = SPRITE_HOOK_ICON;
			m_Color = ColorRGBA(1.0f, 1.0f, 1.0f);
			break;
		case EHookStrongWeakState::WEAK:
			m_Sprite = SPRITE_HOOK_WEAK;
			m_Color = color_cast<ColorRGBA>(ColorHSLA(41131));
			break;
		}
		m_Color.a = Data.m_Color.a;
	}

public:
	CNamePlatePartHookStrongWeak(CGameClient &This) :
		CNamePlatePartSprite(This)
	{
		m_Texture = g_pData->m_aImages[IMAGE_STRONGWEAK].m_Id;
		m_Padding = vec2(0.0f, 0.0f);
	}
};

class CNamePlatePartHookStrongWeakId : public CNamePlatePartText
{
private:
	int m_StrongWeakId = -1;
	static_assert(MAX_CLIENTS <= 999, "Make this buffer bigger");
	char m_aText[4] = "";
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_ShowHookStrongWeakId;
		if(!m_Visible)
			return false;
		switch(Data.m_HookStrongWeakState)
		{
		case EHookStrongWeakState::STRONG:
			m_Color = color_cast<ColorRGBA>(ColorHSLA(6401973));
			break;
		case EHookStrongWeakState::NEUTRAL:
			m_Color = ColorRGBA(1.0f, 1.0f, 1.0f);
			break;
		case EHookStrongWeakState::WEAK:
			m_Color = color_cast<ColorRGBA>(ColorHSLA(41131));
			break;
		}
		m_Color.a = Data.m_Color.a;
		return m_FontSize != Data.m_FontSizeHookStrongWeak || m_StrongWeakId != Data.m_HookStrongWeakId;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeHookStrongWeak;
		m_StrongWeakId = Data.m_HookStrongWeakId;
		str_format(m_aText, sizeof(m_aText), "%d", m_StrongWeakId);
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartHookStrongWeakId(CGameClient &This) :
		CNamePlatePartText(This) {}
};

// ***** TClient Parts *****

class CNamePlatePartCountry : public CNamePlatePart
{
protected:
	static constexpr float FLAG_WIDTH = 128.0f;
	static constexpr float FLAG_HEIGHT = 64.0f;
	static constexpr float FLAG_RATIO = FLAG_HEIGHT / FLAG_WIDTH;
	const CCountryFlags::CCountryFlag *m_pCountryFlag = nullptr;
	float m_Alpha = 1.0f;

public:
	friend class CGameClient;
	void Update(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = true;
		if(g_Config.m_TcNameplateCountry == 0)
		{
			m_Visible = false;
			return;
		}
		if(Data.m_InGame)
		{
			// Check for us and dummy, Data.m_Local only does current char
			for(const auto Id : This.m_aLocalIds)
			{
				if(Id == Data.m_ClientId)
				{
					m_Visible = false;
					return;
				}
			}
			m_pCountryFlag = &This.m_CountryFlags.GetByCountryCode(This.m_aClients[Data.m_ClientId].m_Country);
		}
		else
		{
			if(Data.m_ClientId == 0)
				m_pCountryFlag = &This.m_CountryFlags.GetByCountryCode(g_Config.m_PlayerCountry);
			else
				m_pCountryFlag = &This.m_CountryFlags.GetByCountryCode(g_Config.m_ClDummyCountry);
		}
		// Do not show default flags
		if(m_pCountryFlag == &This.m_CountryFlags.GetByCountryCode(0))
		{
			m_Visible = false;
			return;
		}
		m_Alpha = Data.m_Color.a;
		m_Size = vec2(Data.m_FontSize / FLAG_RATIO, Data.m_FontSize);
	}
	void Render(CGameClient &This, vec2 Pos) const override
	{
		if(!m_pCountryFlag)
			return;
		This.m_CountryFlags.Render(*m_pCountryFlag, ColorRGBA(1.0f, 1.0f, 1.0f, m_Alpha),
			Pos.x - m_Size.x / 2.0f, Pos.y - m_Size.y / 2.0f,
			m_Size.x, m_Size.y);
	}
	CNamePlatePartCountry(CGameClient &This) :
		CNamePlatePart(This) {}
};

class CNamePlatePartPing : public CNamePlatePart
{
protected:
	float m_Radius = 7.0f;
	ColorRGBA m_Color;

public:
	friend class CGameClient;
	void Update(CGameClient &This, const CNamePlateData &Data) override
	{
		const CNetObj_PlayerInfo *pInfo = Data.m_InGame ? This.m_Snap.m_apPlayerInfos[Data.m_ClientId] : nullptr;
		/*
			If in a real game,
				Show other people's pings if in scoreboard
				Or if ping circle and name enabled
			If in preview
				Show ping if ping circle and name enabled
		*/
		m_Radius = Data.m_FontSize / 3.0f;
		m_Size = vec2(m_Radius, m_Radius) * 1.5f;
		m_Visible = Data.m_InGame ? (
						    (g_Config.m_TcNameplatePingCircle > 0 ||
							    (This.m_Scoreboard.IsActive() && pInfo && !pInfo->m_Local))) :
					    (g_Config.m_TcNameplatePingCircle > 0);
		if(!m_Visible)
			return;
		int Ping = Data.m_InGame && pInfo ? pInfo->m_Latency : (1 + Data.m_ClientId) * 25;
		m_Color = color_cast<ColorRGBA>(ColorHSLA((float)(300 - std::clamp(Ping, 0, 300)) / 1000.0f, 1.0f, 0.5f, Data.m_Color.a));
	}
	void Render(CGameClient &This, vec2 Pos) const override
	{
		This.Graphics()->TextureClear();
		This.Graphics()->QuadsBegin();
		This.Graphics()->SetColor(m_Color);
		This.Graphics()->DrawCircle(Pos.x, Pos.y, m_Radius, 24);
		This.Graphics()->QuadsEnd();
	}
	CNamePlatePartPing(CGameClient &This) :
		CNamePlatePart(This) {}
};

class CNamePlatePartSkin : public CNamePlatePartText
{
private:
	char m_aText[MAX_SKIN_LENGTH] = "";
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		const CNetObj_PlayerInfo *pInfo = Data.m_InGame ? This.m_Snap.m_apPlayerInfos[Data.m_ClientId] : nullptr;
		m_Visible = Data.m_InGame ? (pInfo && g_Config.m_TcNameplateSkins > (pInfo->m_Local ? 1 : 0)) : g_Config.m_TcNameplateSkins > 0;
		if(Data.m_InGame && This.ShouldHideStreamerSkin(Data.m_ClientId))
			m_Visible = false;
		if(!m_Visible)
			return false;
		m_Color = Data.m_Color;
		const char *pSkin = Data.m_InGame ? This.m_aClients[Data.m_ClientId].m_aSkinName : (Data.m_ClientId == 0 ? g_Config.m_ClPlayerSkin : g_Config.m_ClDummySkin);
		return m_FontSize != Data.m_FontSizeClan || str_comp(m_aText, pSkin) != 0;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeClan;
		const char *pSkin = Data.m_InGame ? This.m_aClients[Data.m_ClientId].m_aSkinName : (Data.m_ClientId == 0 ? g_Config.m_ClPlayerSkin : g_Config.m_ClDummySkin);
		str_copy(m_aText, pSkin, sizeof(m_aText));
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartSkin(CGameClient &This) :
		CNamePlatePartText(This) {}
};

class CNamePlatePartCoordinates : public CNamePlatePartText
{
private:
	float m_Coord = INFINITY;
	float m_FontSize = -INFINITY;
	bool m_Show = false;
	bool m_IsX = false;
	bool m_Aligned = false;
	char m_aText[32] = "";

	static int RoundCoordToCentitiles(float Value)
	{
		return round_to_int(Value * 100.0f);
	}

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		const bool Show = Data.m_ShowCoords && (m_IsX ? Data.m_ShowCoordX : Data.m_ShowCoordY);
		m_Visible = Show;
		if(!m_Visible)
			return false;
		const bool Aligned = m_IsX && Data.m_CoordXAligned;
		m_Color = Aligned ? Data.m_CoordXAlignColor : Data.m_Color;
		const float Coord = RoundCoordToCentitiles(m_IsX ? Data.m_Coords.x : Data.m_Coords.y) / 100.0f;
		return m_FontSize != Data.m_FontSizeCoords || m_Show != Show || m_Aligned != Aligned || m_Coord != Coord;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeCoords;
		m_Show = Data.m_ShowCoords && (m_IsX ? Data.m_ShowCoordX : Data.m_ShowCoordY);
		m_Aligned = m_IsX && Data.m_CoordXAligned;
		m_Coord = RoundCoordToCentitiles(m_IsX ? Data.m_Coords.x : Data.m_Coords.y) / 100.0f;
		str_format(m_aText, sizeof(m_aText), "%c:%.2f", m_IsX ? 'X' : 'Y', m_Coord);

		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartCoordinates(CGameClient &This, bool IsX) :
		CNamePlatePartText(This),
		m_IsX(IsX) {}
};

class CNamePlatePartReason : public CNamePlatePartText
{
private:
	char m_aText[MAX_WARLIST_REASON_LENGTH] = "";
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = Data.m_InGame && g_Config.m_TcWarList != 0 && g_Config.m_TcWarListReason != 0;
		if(!m_Visible)
			return false;
		const CNetObj_PlayerInfo *pInfo = This.m_Snap.m_apPlayerInfos[Data.m_ClientId];
		const char *pReason = This.m_WarList.GetWarData(Data.m_ClientId).m_aReason;
		m_Visible = pInfo && pReason[0] != '\0' && !pInfo->m_Local;
		if(!m_Visible)
			return false;
		m_Color = ColorRGBA(0.7f, 0.7f, 0.7f, Data.m_Color.a);
		return m_FontSize != Data.m_FontSizeClan || str_comp(m_aText, pReason) != 0;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSizeClan;
		const char *pReason = This.m_WarList.GetWarData(Data.m_ClientId).m_aReason;
		str_copy(m_aText, pReason, sizeof(m_aText));
		CTextCursor Cursor;
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, m_aText);
	}

public:
	CNamePlatePartReason(CGameClient &This) :
		CNamePlatePartText(This) {}
};

class CNamePlatePartIgnoreMark : public CNamePlatePartText
{
private:
	float m_FontSize = -INFINITY;

protected:
	bool UpdateNeeded(CGameClient &This, const CNamePlateData &Data) override
	{
		m_Visible = (Data.m_InGame && This.Client()->State() != IClient::STATE_DEMOPLAYBACK && (This.m_aClients[Data.m_ClientId].m_Foe || This.m_aClients[Data.m_ClientId].m_ChatIgnore));
		if(!m_Visible)
			return false;
		m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, Data.m_Color.a);
		return m_FontSize != Data.m_FontSize;
	}
	void UpdateText(CGameClient &This, const CNamePlateData &Data) override
	{
		m_FontSize = Data.m_FontSize;
		CTextCursor Cursor;
		This.TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		Cursor.m_FontSize = m_FontSize;
		This.TextRender()->CreateOrAppendTextContainer(m_TextContainerIndex, &Cursor, FontIcons::FONT_ICON_COMMENT_SLASH);
		This.TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	}

public:
	CNamePlatePartIgnoreMark(CGameClient &This) :
		CNamePlatePartText(This) {}
};

// ***** Name Plates *****

class CNamePlate
{
private:
	struct SCoreRowParts
	{
		ENameplateCoreRow m_Row = ENameplateCoreRow::NUM_ROWS;
		size_t m_Start = 0;
		size_t m_End = 0;
	};

	bool m_Inited = false;
	bool m_InGame = false;
	PartsVector m_vpParts;
	std::vector<SCoreRowParts> m_vCoreRows;
	void RenderLine(CGameClient &This,
		vec2 Pos, vec2 Size,
		PartsVector::iterator Start, PartsVector::iterator End)
	{
		Pos.x -= Size.x / 2.0f;
		for(auto PartIt = Start; PartIt != End; ++PartIt)
		{
			const CNamePlatePart &Part = **PartIt;
			if(Part.Visible())
			{
				Part.Render(This, vec2(
							  Pos.x + (Part.Padding().x + Part.Size().x) / 2.0f,
							  Pos.y - std::max(Size.y, Part.Padding().y + Part.Size().y) / 2.0f));
			}
			if(Part.Visible() || Part.ShiftOnInvis())
				Pos.x += Part.Size().x + Part.Padding().x;
		}
	}
	vec2 RangeSize(size_t StartIndex, size_t EndIndex) const
	{
		vec2 LineSize = vec2(0.0f, 0.0f);
		float WidthMax = 0.0f;
		float HeightTotal = 0.0f;
		bool Empty = true;
		for(size_t PartIndex = StartIndex; PartIndex < EndIndex; ++PartIndex)
		{
			const CNamePlatePart &Part = *m_vpParts[PartIndex];
			if(Part.NewLine())
			{
				if(!Empty)
				{
					WidthMax = std::max(WidthMax, LineSize.x);
					HeightTotal += LineSize.y;
				}
				LineSize = vec2(0.0f, 0.0f);
			}
			else if(Part.Visible() || Part.ShiftOnInvis())
			{
				Empty = false;
				LineSize.x += Part.Size().x + Part.Padding().x;
				LineSize.y = std::max(LineSize.y, Part.Size().y + Part.Padding().y);
			}
		}
		WidthMax = std::max(WidthMax, LineSize.x);
		HeightTotal += LineSize.y;
		return vec2(WidthMax, HeightTotal);
	}
	vec2 CoreRowSize(const SCoreRowParts &CoreRow) const
	{
		return RangeSize(CoreRow.m_Start, CoreRow.m_End);
	}
	vec2 LayoutCoreRowSize(const SCoreRowParts &CoreRow, const CNamePlate *pLayoutReference) const
	{
		if(pLayoutReference == nullptr)
			return CoreRowSize(CoreRow);
		for(const SCoreRowParts &ReferenceCoreRow : pLayoutReference->m_vCoreRows)
		{
			if(ReferenceCoreRow.m_Row == CoreRow.m_Row)
				return pLayoutReference->CoreRowSize(ReferenceCoreRow);
		}
		return CoreRowSize(CoreRow);
	}
	float RangeTopY(vec2 PositionBottomMiddle, size_t StartIndex, size_t EndIndex) const
	{
		vec2 Position = PositionBottomMiddle;
		vec2 LineSize = vec2(0.0f, 0.0f);
		bool Empty = true;
		float Top = Position.y;
		for(size_t PartIndex = StartIndex; PartIndex < EndIndex; ++PartIndex)
		{
			const CNamePlatePart &Part = *m_vpParts[PartIndex];
			if(Part.NewLine())
			{
				if(!Empty)
				{
					Top = std::min(Top, Position.y - LineSize.y / 2.0f);
					Position.y -= LineSize.y;
				}
				LineSize = vec2(0.0f, 0.0f);
			}
			else if(Part.Visible() || Part.ShiftOnInvis())
			{
				Empty = false;
				LineSize.x += Part.Size().x + Part.Padding().x;
				LineSize.y = std::max(LineSize.y, Part.Size().y + Part.Padding().y);
			}
		}
		return std::min(Top, Position.y - LineSize.y / 2.0f);
	}
	void RenderRange(CGameClient &This, vec2 PositionBottomMiddle, size_t StartIndex, size_t EndIndex)
	{
		vec2 Position = PositionBottomMiddle;
		vec2 LineSize = vec2(0.0f, 0.0f);
		bool Empty = true;
		auto Start = m_vpParts.begin() + StartIndex;
		for(auto PartIt = Start; PartIt != m_vpParts.begin() + EndIndex; ++PartIt)
		{
			CNamePlatePart &Part = **PartIt;
			if(Part.NewLine())
			{
				if(!Empty)
				{
					RenderLine(This, Position, LineSize, Start, std::next(PartIt));
					Position.y -= LineSize.y;
				}
				Start = std::next(PartIt);
				LineSize = vec2(0.0f, 0.0f);
			}
			else if(Part.Visible() || Part.ShiftOnInvis())
			{
				Empty = false;
				LineSize.x += Part.Size().x + Part.Padding().x;
				LineSize.y = std::max(LineSize.y, Part.Size().y + Part.Padding().y);
			}
		}
		RenderLine(This, Position, LineSize, Start, m_vpParts.begin() + EndIndex);
	}

public:
	// Compute the baseline rect of the row TargetRow (offset = 0), and its
	// center. Also returns the nameplate's baseline frame (Min/Max), which is
	// the stack bbox of all visible rows with offsets ignored. The frame is
	// stable regardless of how rows are dragged; rows are clamped to it.
	void ComputeBaselineLayout(vec2 PositionBottomMiddle, ENameplateCoreRow TargetRow,
		bool &HasFrame, vec2 &FrameMin, vec2 &FrameMax,
		bool &HasTargetRow, vec2 &TargetRowCenter, vec2 &TargetRowSize,
		const CNamePlate *pLayoutReference = nullptr) const
	{
		HasFrame = false;
		HasTargetRow = false;
		vec2 Position = PositionBottomMiddle;
		for(const SCoreRowParts &CoreRow : m_vCoreRows)
		{
			const vec2 Size = CoreRowSize(CoreRow);
			const vec2 LayoutSize = LayoutCoreRowSize(CoreRow, pLayoutReference);
			if(Size.x > 0.0f && Size.y > 0.0f)
			{
				// Baseline rect (no offset).
				const vec2 RowMin = vec2(Position.x - Size.x / 2.0f, Position.y - Size.y);
				const vec2 RowMax = vec2(Position.x + Size.x / 2.0f, Position.y);
				if(!HasFrame)
				{
					FrameMin = RowMin;
					FrameMax = RowMax;
					HasFrame = true;
				}
				else
				{
					FrameMin.x = std::min(FrameMin.x, RowMin.x);
					FrameMin.y = std::min(FrameMin.y, RowMin.y);
					FrameMax.x = std::max(FrameMax.x, RowMax.x);
					FrameMax.y = std::max(FrameMax.y, RowMax.y);
				}
				if(CoreRow.m_Row == TargetRow)
				{
					TargetRowCenter = vec2(Position.x, Position.y - Size.y / 2.0f);
					TargetRowSize = Size;
					HasTargetRow = true;
				}
			}
			Position.y -= LayoutSize.y;
		}
	}
	// Returns the baseline frame only.
	void ComputeBaselineFrame(vec2 PositionBottomMiddle, bool &HasFrame, vec2 &FrameMin, vec2 &FrameMax) const
	{
		bool DummyHasRow = false;
		vec2 DummyCenter, DummySize;
		ComputeBaselineLayout(PositionBottomMiddle, ENameplateCoreRow::NUM_ROWS,
			HasFrame, FrameMin, FrameMax, DummyHasRow, DummyCenter, DummySize);
	}

private:
	template<typename PartType, typename... ArgsType>
	void AddPart(CGameClient &This, ArgsType &&...Args)
	{
		m_vpParts.push_back(std::make_unique<PartType>(This, std::forward<ArgsType>(Args)...));
	}

	void AddNameRow(CGameClient &This)
	{
		AddPart<CNamePlatePartCountry>(This); // TClient
		AddPart<CNamePlatePartPing>(This); // TClient
		AddPart<CNamePlatePartIgnoreMark>(This); // TClient
		AddPart<CNamePlatePartFriendMark>(This);
		AddPart<CNamePlatePartClientId>(This, false);
		AddPart<CNamePlatePartName>(This);
		AddPart<CNamePlatePartNewLine>(This);
	}

	void AddClanModule(CGameClient &This)
	{
		AddPart<CNamePlatePartClan>(This);
		AddPart<CNamePlatePartNewLine>(This);

		// Keep legacy optional rows attached to the clan module.
		AddPart<CNamePlatePartReason>(This); // TClient
		AddPart<CNamePlatePartNewLine>(This); // TClient
		AddPart<CNamePlatePartSkin>(This); // TClient
		AddPart<CNamePlatePartNewLine>(This); // TClient
		AddPart<CNamePlatePartClientId>(This, true);
		AddPart<CNamePlatePartNewLine>(This);
	}

	void AddHookRow(CGameClient &This)
	{
		AddPart<CNamePlatePartHookStrongWeak>(This);
		AddPart<CNamePlatePartHookStrongWeakId>(This);
		AddPart<CNamePlatePartNewLine>(This);
	}

	void AddCoordsRow(CGameClient &This)
	{
		AddPart<CNamePlatePartCoordinates>(This, true); // TClient
		AddPart<CNamePlatePartCoordinates>(This, false); // TClient
		AddPart<CNamePlatePartNewLine>(This);
	}

	void AddKeysRow(CGameClient &This)
	{
		AddPart<CNamePlatePartDirection>(This, DIRECTION_LEFT);
		AddPart<CNamePlatePartDirection>(This, DIRECTION_UP);
		AddPart<CNamePlatePartDirection>(This, DIRECTION_RIGHT);
		AddPart<CNamePlatePartNewLine>(This);
	}

	void AddCoreRowModule(CGameClient &This, ENameplateCoreRow Row)
	{
		switch(Row)
		{
		case ENameplateCoreRow::NAME:
			AddNameRow(This);
			break;
		case ENameplateCoreRow::CLAN:
			AddClanModule(This);
			break;
		case ENameplateCoreRow::HOOK:
			AddHookRow(This);
			break;
		case ENameplateCoreRow::COORDS:
			AddCoordsRow(This);
			break;
		case ENameplateCoreRow::KEYS:
			AddKeysRow(This);
			break;
		case ENameplateCoreRow::NUM_ROWS:
			break;
		}
	}

	void RebuildPartLayout(CGameClient &This)
	{
		for(auto &Part : m_vpParts)
			Part->Reset(This);
		m_vpParts.clear();
		m_vCoreRows.clear();

		for(auto It = s_aDefaultNameplateCoreRowsTopToBottom.rbegin(); It != s_aDefaultNameplateCoreRowsTopToBottom.rend(); ++It)
		{
			const size_t StartIndex = m_vpParts.size();
			AddCoreRowModule(This, *It);
			m_vCoreRows.push_back({*It, StartIndex, m_vpParts.size()});
		}
	}

	void Init(CGameClient &This)
	{
		if(m_Inited)
			return;

		RebuildPartLayout(This);
		m_Inited = true;
	}

public:
	CNamePlate() = default;
	CNamePlate(CGameClient &This, const CNamePlateData &Data)
	{
		// Convenience constructor
		Update(This, Data);
	}
	void Reset(CGameClient &This)
	{
		for(auto &Part : m_vpParts)
			Part->Reset(This);
	}
	void Update(CGameClient &This, const CNamePlateData &Data)
	{
		Init(This);
		m_InGame = Data.m_InGame;
		for(auto &Part : m_vpParts)
			Part->Update(This, Data);
	}
	void Render(CGameClient &This, const vec2 &PositionBottomMiddle, const CNamePlate *pLayoutReference = nullptr)
	{
		dbg_assert(m_Inited, "Tried to render uninited nameplate");
		if(NameplateFreeMoveEnabled())
		{
			// Each row is positioned independently from PositionBottomMiddle:
			// row top = baseline top (stack default) + Offset(row). This
			// guarantees moving one row never shifts another.
			vec2 BaselinePos = PositionBottomMiddle;
			for(const SCoreRowParts &CoreRow : m_vCoreRows)
			{
				const vec2 Size = CoreRowSize(CoreRow);
				const vec2 LayoutSize = LayoutCoreRowSize(CoreRow, pLayoutReference);
				if(Size.x > 0.0f && Size.y > 0.0f)
					RenderRange(This, BaselinePos + NameplateCoreRowOffset(CoreRow.m_Row), CoreRow.m_Start, CoreRow.m_End);
				BaselinePos.y -= LayoutSize.y;
			}
			This.Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			return;
		}
		RenderRange(This, PositionBottomMiddle, 0, m_vpParts.size());
		This.Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	bool IsInitialized() const
	{
		return m_Inited;
	}
	float TopY(const vec2 &PositionBottomMiddle) const
	{
		dbg_assert(m_Inited, "Tried to get top of uninited nameplate");
		if(NameplateFreeMoveEnabled())
		{
			// Use the baseline frame top so chat bubbles etc. anchor to a
			// stable, drag-independent position. The drag area is visually
			// larger (see NAMEPLATE_FREE_MOVE_FRAME_SCALE), but TopY is for
			// layout neighbors that should hug the actual content.
			bool HasFrame = false;
			vec2 FrameMin = vec2(0.0f, 0.0f);
			vec2 FrameMax = vec2(0.0f, 0.0f);
			ComputeBaselineFrame(PositionBottomMiddle, HasFrame, FrameMin, FrameMax);
			return HasFrame ? FrameMin.y : PositionBottomMiddle.y;
		}

		return RangeTopY(PositionBottomMiddle, 0, m_vpParts.size());
	}
	vec2 Size() const
	{
		dbg_assert(m_Inited, "Tried to get size of uninited nameplate");
		if(NameplateFreeMoveEnabled())
		{
			// Reported size stays at the baseline stack bbox so layout code
			// (chat bubbles, preview-page tee placement) hugs the actual
			// content. The drag area is scaled separately at draw time.
			bool HasFrame = false;
			vec2 FrameMin = vec2(0.0f, 0.0f);
			vec2 FrameMax = vec2(0.0f, 0.0f);
			ComputeBaselineFrame(vec2(0.0f, 0.0f), HasFrame, FrameMin, FrameMax);
			return HasFrame ? FrameMax - FrameMin : vec2(0.0f, 0.0f);
		}
		return RangeSize(0, m_vpParts.size());
	}
	void CollectCoreRowRects(vec2 PositionBottomMiddle, std::array<SNameplateCoreRowRect, kNameplateCoreRowCount> &aRects, const CNamePlate *pLayoutReference = nullptr) const
	{
		for(SNameplateCoreRowRect &Rect : aRects)
			Rect = SNameplateCoreRowRect();

		vec2 BaselinePos = PositionBottomMiddle;
		for(const SCoreRowParts &CoreRow : m_vCoreRows)
		{
			const vec2 Size = CoreRowSize(CoreRow);
			const vec2 LayoutSize = LayoutCoreRowSize(CoreRow, pLayoutReference);
			if(Size.x > 0.0f && Size.y > 0.0f)
			{
				SNameplateCoreRowRect &Rect = aRects[static_cast<int>(CoreRow.m_Row)];
				const vec2 RowPosition = BaselinePos + NameplateCoreRowOffset(CoreRow.m_Row);
				Rect.m_Row = CoreRow.m_Row;
				Rect.m_Min = vec2(RowPosition.x - Size.x / 2.0f, RowPosition.y - Size.y);
				Rect.m_Max = vec2(RowPosition.x + Size.x / 2.0f, RowPosition.y);
				Rect.m_Visible = true;
			}
			BaselinePos.y -= LayoutSize.y;
		}
	}
};

class CNamePlates::CNamePlatesData
{
public:
	CNamePlate m_aNamePlates[MAX_CLIENTS];
	CNamePlate m_aNamePlateFrameReferences[MAX_CLIENTS];
	SChatBubbleAnimState m_aChatBubbleAnim[MAX_CLIENTS];
	SCoordXAlignState m_aCoordXAlign[MAX_CLIENTS];
	SCoordXAlignFrameState m_CoordXAlignFrame;
	ENameplateCoreRow m_FreeMoveDragRow = ENameplateCoreRow::NUM_ROWS;
	vec2 m_FreeMoveDragStartMouse = vec2(0.0f, 0.0f);
	vec2 m_FreeMoveDragStartOffset = vec2(0.0f, 0.0f);
};

static bool NameplateCoreRowRectContains(const SNameplateCoreRowRect &Rect, vec2 Position)
{
	return Rect.m_Visible &&
	       Position.x >= Rect.m_Min.x &&
	       Position.x <= Rect.m_Max.x &&
	       Position.y >= Rect.m_Min.y &&
	       Position.y <= Rect.m_Max.y;
}

static int RoundCoordToCentitiles(float Value)
{
	return round_to_int(Value * 100.0f);
}

void CNamePlates::UpdateCoordXAlignFrameState()
{
	SCoordXAlignFrameState &FrameState = m_pData->m_CoordXAlignFrame;
	FrameState = SCoordXAlignFrameState();

	if(g_Config.m_QmNameplateCoordXAlignHint == 0 && g_Config.m_QmNameplateCoordXAlignHintStrict == 0)
	{
		for(SCoordXAlignState &CoordXAlignState : m_pData->m_aCoordXAlign)
			CoordXAlignState = SCoordXAlignState();
		return;
	}
	if(GameClient()->m_Snap.m_LocalClientId < 0 || GameClient()->m_Snap.m_LocalClientId >= MAX_CLIENTS)
	{
		for(SCoordXAlignState &CoordXAlignState : m_pData->m_aCoordXAlign)
			CoordXAlignState = SCoordXAlignState();
		return;
	}
	if(!GameClient()->m_Snap.m_aCharacters[GameClient()->m_Snap.m_LocalClientId].m_Active)
	{
		for(SCoordXAlignState &CoordXAlignState : m_pData->m_aCoordXAlign)
			CoordXAlignState = SCoordXAlignState();
		return;
	}

	FrameState.m_LocalClientId = GameClient()->m_Snap.m_LocalClientId;
	FrameState.m_LocalRoundedX = RoundCoordToCentitiles(GameClient()->m_LocalCharacterPos.x / 32.0f);

	const int MaxAllowedDiff = g_Config.m_QmNameplateCoordXAlignHintStrict ? 0 : 3;
	const float WindowSeconds = std::clamp(g_Config.m_QmNameplateCoordXAlignHintWindowMs / 1000.0f, 0.1f, 3.0f);
	const float Now = Client()->LocalTime();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == FrameState.m_LocalClientId || !GameClient()->m_Snap.m_apPlayerInfos[ClientId])
			continue;
		if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active || GameClient()->m_aClients[ClientId].m_IsVolleyBall)
			continue;
		if(GameClient()->IsOtherTeam(ClientId))
		{
			m_pData->m_aCoordXAlign[ClientId] = SCoordXAlignState();
			continue;
		}

		SCoordXAlignState &CoordXAlignState = m_pData->m_aCoordXAlign[ClientId];
		const int RoundedX = RoundCoordToCentitiles(GameClient()->m_aClients[ClientId].m_RenderPos.x / 32.0f);
		const int RoundedXDiff = absolute(RoundedX - FrameState.m_LocalRoundedX);
		if(!CoordXAlignState.m_Active || RoundedXDiff > MaxAllowedDiff)
		{
			CoordXAlignState.m_Active = true;
			CoordXAlignState.m_Aligned = false;
			CoordXAlignState.m_WindowStartTime = Now;
		}
		CoordXAlignState.m_Aligned = RoundedXDiff <= MaxAllowedDiff && Now - CoordXAlignState.m_WindowStartTime >= WindowSeconds;
		if(CoordXAlignState.m_Aligned)
			FrameState.m_LocalAligned = true;
	}
}

void CNamePlates::RenderNamePlateGame(vec2 Position, const CNetObj_PlayerInfo *pPlayerInfo, float Alpha, bool TrackCoordXAlign)
{
	if(!pPlayerInfo)
		return;

	const int ClientId = pPlayerInfo->m_ClientId;
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;

	// Get screen edges to avoid rendering offscreen
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	// Assume that the name plate fits into a 800x800 box placed directly above the tee
	ScreenX0 -= 400;
	ScreenX1 += 400;
	ScreenY1 += 800;
	if(!(in_range(Position.x, ScreenX0, ScreenX1) && in_range(Position.y, ScreenY0, ScreenY1)))
	{
		if(TrackCoordXAlign)
			m_pData->m_aCoordXAlign[ClientId] = SCoordXAlignState();
		return;
	}

	CNamePlateData Data;

	const auto &ClientData = GameClient()->m_aClients[ClientId];
	const bool OtherTeam = GameClient()->IsOtherTeam(ClientId);
	const bool IsAnyLocalClient = GameClient()->IsLocalClientId(ClientId);

	Data.m_InGame = true;

	const bool HideIdentity = GameClient()->ShouldHideStreamerIdentity(ClientId);

	Data.m_ShowName = pPlayerInfo->m_Local ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates;
	GameClient()->FormatStreamerName(ClientId, Data.m_aName, sizeof(Data.m_aName));
	Data.m_ShowFriendMark = Data.m_ShowName && g_Config.m_ClNamePlatesFriendMark && GameClient()->m_aClients[ClientId].m_Friend;
	Data.m_ShowClientId = Data.m_ShowName && (g_Config.m_Debug || g_Config.m_ClNamePlatesIds) && !HideIdentity;
	Data.m_FontSize = 18.0f + 20.0f * g_Config.m_ClNamePlatesSize / 100.0f;

	Data.m_ClientId = ClientId;
	Data.m_ClientIdSeparateLine = g_Config.m_ClNamePlatesIdsSeparateLine;
	Data.m_FontSizeClientId = Data.m_ClientIdSeparateLine ? (18.0f + 20.0f * g_Config.m_ClNamePlatesIdsSize / 100.0f) : Data.m_FontSize;

	Data.m_ShowClan = Data.m_ShowName && g_Config.m_ClNamePlatesClan && !HideIdentity;
	GameClient()->FormatStreamerClan(ClientId, Data.m_aClan, sizeof(Data.m_aClan));
	Data.m_FontSizeClan = 18.0f + 20.0f * g_Config.m_ClNamePlatesClanSize / 100.0f;

	Data.m_CoordXAlignHint = g_Config.m_QmNameplateCoordXAlignHint != 0;
	Data.m_CoordXAlignHintStrict = g_Config.m_QmNameplateCoordXAlignHintStrict != 0;
	Data.m_CoordXAlignColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_QmNameplateCoordXAlignHintColor));

	SCoordXAlignState &CoordXAlignState = m_pData->m_aCoordXAlign[ClientId];
	const bool CoordXAlignHintEnabled =
		(Data.m_CoordXAlignHint || Data.m_CoordXAlignHintStrict);
	const bool LocalCoordXAligned =
		TrackCoordXAlign &&
		ClientId == m_pData->m_CoordXAlignFrame.m_LocalClientId &&
		m_pData->m_CoordXAlignFrame.m_LocalAligned;
	const bool NameplateScopeAllowsCoords = pPlayerInfo->m_Local ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates;
	const bool CoordModuleAllowsCoords = pPlayerInfo->m_Local ? g_Config.m_QmNameplateCoordsOwn : g_Config.m_QmNameplateCoords;
	const bool ShowLocalAlignedCoordX = NameplateScopeAllowsCoords && CoordXAlignHintEnabled && LocalCoordXAligned;
	Data.m_ShowCoordX = (NameplateScopeAllowsCoords && CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordX != 0) || ShowLocalAlignedCoordX;
	Data.m_ShowCoordY = NameplateScopeAllowsCoords && CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordY != 0;
	Data.m_ShowCoords = (NameplateScopeAllowsCoords && CoordModuleAllowsCoords) || ShowLocalAlignedCoordX;
	Data.m_Coords = Position / 32.0f;
	Data.m_FontSizeCoords = 18.0f + 20.0f * g_Config.m_ClNamePlatesCoordsSize / 100.0f;

	const bool ShouldTrackCoordXAlign =
		TrackCoordXAlign &&
		!IsAnyLocalClient &&
		GameClient()->m_Snap.m_LocalClientId >= 0 &&
		!OtherTeam &&
		CoordXAlignHintEnabled;
	if(ShouldTrackCoordXAlign)
	{
		Data.m_CoordXAligned = CoordXAlignState.m_Aligned;
	}
	else
	{
		if(TrackCoordXAlign)
			CoordXAlignState = SCoordXAlignState();
		Data.m_CoordXAligned = LocalCoordXAligned;
	}

	Data.m_FontSizeHookStrongWeak = 18.0f + 20.0f * g_Config.m_ClNamePlatesStrongSize / 100.0f;
	Data.m_FontSizeDirection = 18.0f + 20.0f * g_Config.m_ClDirectionSize / 100.0f;

	if(g_Config.m_ClNamePlatesAlways == 0)
		Alpha *= std::clamp(1.0f - std::pow(distance(GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy], Position) / 200.0f, 16.0f), 0.0f, 1.0f);
	const float LiveObserverAlpha = GameClient()->LiveObserverClientAlpha(ClientId);
	if(LiveObserverAlpha < 1.0f)
		Alpha *= LiveObserverAlpha;
	else if(OtherTeam)
		Alpha *= (float)g_Config.m_ClShowOthersAlpha / 100.0f;
	if(GameClient()->m_FastPractice.Enabled() && !GameClient()->m_Snap.m_SpecInfo.m_Active && !GameClient()->m_FastPractice.IsPracticeParticipant(pPlayerInfo->m_ClientId))
		Alpha = std::min(Alpha, 0.5f);

	Data.m_Color = ColorRGBA(1.0f, 1.0f, 1.0f);
	if(g_Config.m_ClNamePlatesTeamcolors)
	{
		if(GameClient()->IsTeamPlay())
		{
			if(ClientData.m_Team == TEAM_RED)
				Data.m_Color = ColorRGBA(1.0f, 0.5f, 0.5f);
			else if(ClientData.m_Team == TEAM_BLUE)
				Data.m_Color = ColorRGBA(0.7f, 0.7f, 1.0f);
		}
		else
		{
			const int Team = GameClient()->m_Teams.Team(ClientId);
			if(Team)
				Data.m_Color = GameClient()->GetDDTeamColor(Team, 0.75f);
		}
	}
	Data.m_Color.a = Alpha;

	int ShowDirectionConfig = g_Config.m_ClShowDirection;
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current())
		ShowDirectionConfig = g_Config.m_ClVideoShowDirection;
#endif
	Data.m_DirLeft = Data.m_DirJump = Data.m_DirRight = false;
	switch(ShowDirectionConfig)
	{
	case 0: // Off
		Data.m_ShowDirection = false;
		break;
	case 1: // Others
		Data.m_ShowDirection = !pPlayerInfo->m_Local;
		break;
	case 2: // Everyone
		Data.m_ShowDirection = true;
		break;
	case 3: // Only self
		Data.m_ShowDirection = pPlayerInfo->m_Local;
		break;
	default:
		dbg_assert_failed("ShowDirectionConfig invalid");
	}
	if(Data.m_ShowDirection)
	{
		if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
			ClientId == GameClient()->m_aLocalIds[!g_Config.m_ClDummy])
		{
			const auto &InputData = GameClient()->m_Controls.m_aInputData[!g_Config.m_ClDummy];
			Data.m_DirLeft = InputData.m_Direction == -1;
			Data.m_DirJump = InputData.m_Jump == 1;
			Data.m_DirRight = InputData.m_Direction == 1;
		}
		else if(Client()->State() != IClient::STATE_DEMOPLAYBACK && pPlayerInfo->m_Local) // Always render local input when not in demo playback
		{
			const auto &InputData = GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];
			Data.m_DirLeft = InputData.m_Direction == -1;
			Data.m_DirJump = InputData.m_Jump == 1;
			Data.m_DirRight = InputData.m_Direction == 1;
		}
		else
		{
			const auto &Character = GameClient()->m_Snap.m_aCharacters[ClientId];
			Data.m_DirLeft = Character.m_Cur.m_Direction == -1;
			Data.m_DirJump = Character.m_Cur.m_Jumped & 1;
			Data.m_DirRight = Character.m_Cur.m_Direction == 1;
		}
	}

	Data.m_ShowHookStrongWeak = false;
	Data.m_HookStrongWeakState = EHookStrongWeakState::NEUTRAL;
	Data.m_ShowHookStrongWeakId = false;
	Data.m_HookStrongWeakId = 0;

	const bool Following = (GameClient()->m_Snap.m_SpecInfo.m_Active && !GameClient()->m_MultiViewActivated && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW);
	if(GameClient()->m_Snap.m_LocalClientId != -1 || Following)
	{
		const int SelectedId = Following ? GameClient()->m_Snap.m_SpecInfo.m_SpectatorId : GameClient()->m_Snap.m_LocalClientId;
		if(SelectedId >= 0 && SelectedId < MAX_CLIENTS)
		{
			const CGameClient::CSnapState::CCharacterInfo &Selected = GameClient()->m_Snap.m_aCharacters[SelectedId];
			const CGameClient::CSnapState::CCharacterInfo &Other = GameClient()->m_Snap.m_aCharacters[ClientId];

			if((Selected.m_HasExtendedData || GameClient()->m_aClients[SelectedId].m_SpecCharPresent) && Other.m_HasExtendedData)
			{
				int SelectedStrongWeakId = Selected.m_HasExtendedData ? Selected.m_ExtendedData.m_StrongWeakId : 0;
				Data.m_HookStrongWeakId = Other.m_ExtendedData.m_StrongWeakId;
				Data.m_ShowHookStrongWeakId = g_Config.m_Debug || g_Config.m_ClNamePlatesStrong == 2;
				if(SelectedId == ClientId)
					Data.m_ShowHookStrongWeak = Data.m_ShowHookStrongWeakId;
				else
				{
					Data.m_HookStrongWeakState = SelectedStrongWeakId > Other.m_ExtendedData.m_StrongWeakId ? EHookStrongWeakState::STRONG : EHookStrongWeakState::WEAK;
					Data.m_ShowHookStrongWeak = g_Config.m_Debug || g_Config.m_ClNamePlatesStrong > 0;
				}
			}
		}
	}

	// TClient
	if(Data.m_ShowName && !HideIdentity && g_Config.m_TcWarList && g_Config.m_TcWarListShowClan && GameClient()->m_WarList.GetWarData(ClientId).m_WarClan)
		Data.m_ShowClan = true;
	Data.m_Local = pPlayerInfo->m_Local;

	CNamePlate *pLayoutReference = nullptr;
	if(Alpha > 0.0f && NameplateFreeMoveEnabled() && (!g_Config.m_ClNamePlates || !g_Config.m_ClNamePlatesOwn))
	{
		CNamePlateData FrameData = Data;
		FrameData.m_ShowName = true;
		FrameData.m_ShowFriendMark = FrameData.m_ShowName && g_Config.m_ClNamePlatesFriendMark && GameClient()->m_aClients[ClientId].m_Friend;
		FrameData.m_ShowClientId = FrameData.m_ShowName && (g_Config.m_Debug || g_Config.m_ClNamePlatesIds) && !HideIdentity;
		FrameData.m_ShowClan = FrameData.m_ShowName && g_Config.m_ClNamePlatesClan && !HideIdentity;
		const bool FrameShowLocalAlignedCoordX = CoordXAlignHintEnabled && LocalCoordXAligned;
		FrameData.m_ShowCoordX = (CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordX != 0) || FrameShowLocalAlignedCoordX;
		FrameData.m_ShowCoordY = CoordModuleAllowsCoords && g_Config.m_QmNameplateCoordY != 0;
		FrameData.m_ShowCoords = CoordModuleAllowsCoords || FrameShowLocalAlignedCoordX;
		if(FrameData.m_ShowName && !HideIdentity && g_Config.m_TcWarList && g_Config.m_TcWarListShowClan && GameClient()->m_WarList.GetWarData(ClientId).m_WarClan)
			FrameData.m_ShowClan = true;

		CNamePlate &FrameNamePlate = m_pData->m_aNamePlateFrameReferences[ClientId];
		FrameNamePlate.Update(*GameClient(), FrameData);
		pLayoutReference = &FrameNamePlate;
	}

	// Check if the nameplate is actually on screen
	CNamePlate &NamePlate = m_pData->m_aNamePlates[ClientId];
	NamePlate.Update(*GameClient(), Data);
	if(Alpha > 0.0f)
		NamePlate.Render(*GameClient(), Position - vec2(0.0f, (float)g_Config.m_ClNamePlatesOffset), pLayoutReference);
}

void CNamePlates::RenderNamePlatePreview(vec2 Position, int Dummy)
{
	const float FontSize = 18.0f + 20.0f * g_Config.m_ClNamePlatesSize / 100.0f;
	const float FontSizeClan = 18.0f + 20.0f * g_Config.m_ClNamePlatesClanSize / 100.0f;
	const float FontSizeCoords = 18.0f + 20.0f * g_Config.m_ClNamePlatesCoordsSize / 100.0f;

	const float FontSizeDirection = 18.0f + 20.0f * g_Config.m_ClDirectionSize / 100.0f;
	const float FontSizeHookStrongWeak = 18.0f + 20.0f * g_Config.m_ClNamePlatesStrongSize / 100.0f;

	auto BuildPreviewData = [&](int DummyIdx, CNamePlateData &Data, bool ForceNameplateScopeAll = false) {
		Data.m_InGame = false;
		Data.m_Color = g_Config.m_ClNamePlatesTeamcolors ? GameClient()->GetDDTeamColor(13, 0.75f) : TextRender()->DefaultTextColor();
		Data.m_Color.a = 1.0f;
		const bool IsOwnPreview = DummyIdx == 0;
		const bool NameplateScopeAllowsPreview = ForceNameplateScopeAll || (IsOwnPreview ? g_Config.m_ClNamePlatesOwn : g_Config.m_ClNamePlates);
		const bool CoordModuleAllowsPreview = IsOwnPreview ? g_Config.m_QmNameplateCoordsOwn : g_Config.m_QmNameplateCoords;

		Data.m_ShowName = NameplateScopeAllowsPreview;
		const char *pName = DummyIdx == 0 ? Client()->PlayerName() : Client()->DummyName();
		str_copy(Data.m_aName, str_utf8_skip_whitespaces(pName));
		str_utf8_trim_right(Data.m_aName);
		Data.m_FontSize = FontSize;

		Data.m_ShowFriendMark = Data.m_ShowName && g_Config.m_ClNamePlatesFriendMark;

		Data.m_ShowClientId = Data.m_ShowName && (g_Config.m_Debug || g_Config.m_ClNamePlatesIds);
		Data.m_ClientId = DummyIdx;
		Data.m_ClientIdSeparateLine = g_Config.m_ClNamePlatesIdsSeparateLine;
		Data.m_FontSizeClientId = Data.m_ClientIdSeparateLine ? (18.0f + 20.0f * g_Config.m_ClNamePlatesIdsSize / 100.0f) : Data.m_FontSize;

		Data.m_ShowClan = Data.m_ShowName && g_Config.m_ClNamePlatesClan;
		const char *pClan = DummyIdx == 0 ? g_Config.m_PlayerClan : g_Config.m_ClDummyClan;
		str_copy(Data.m_aClan, str_utf8_skip_whitespaces(pClan));
		str_utf8_trim_right(Data.m_aClan);
		if(Data.m_aClan[0] == '\0')
			str_copy(Data.m_aClan, Localize("Clan Name"));
		Data.m_FontSizeClan = FontSizeClan;

		Data.m_ShowCoords = NameplateScopeAllowsPreview && CoordModuleAllowsPreview;
		Data.m_ShowCoordX = Data.m_ShowCoords && g_Config.m_QmNameplateCoordX != 0;
		Data.m_ShowCoordY = Data.m_ShowCoords && g_Config.m_QmNameplateCoordY != 0;
		Data.m_Coords = vec2(12.34f + DummyIdx, 56.78f + DummyIdx);
		Data.m_FontSizeCoords = FontSizeCoords;
		Data.m_CoordXAlignHint = false;
		Data.m_CoordXAlignHintStrict = false;
		Data.m_CoordXAligned = false;
		Data.m_CoordXAlignColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_QmNameplateCoordXAlignHintColor));

		switch(g_Config.m_ClShowDirection)
		{
		case 0: // Off
			Data.m_ShowDirection = false;
			break;
		case 1: // Others
			Data.m_ShowDirection = !IsOwnPreview;
			break;
		case 2: // Everyone
			Data.m_ShowDirection = true;
			break;
		case 3: // Only self
			Data.m_ShowDirection = IsOwnPreview;
			break;
		default:
			Data.m_ShowDirection = false;
			dbg_assert_failed("ClShowDirection invalid");
		}
		Data.m_DirLeft = Data.m_DirJump = Data.m_DirRight = true;
		Data.m_FontSizeDirection = FontSizeDirection;

		Data.m_FontSizeHookStrongWeak = FontSizeHookStrongWeak;
		Data.m_HookStrongWeakId = Data.m_ClientId;
		Data.m_ShowHookStrongWeakId = NameplateScopeAllowsPreview && g_Config.m_ClNamePlatesStrong == 2;
		const bool IsPlayerPreview = IsOwnPreview;
		Data.m_HookStrongWeakState = IsPlayerPreview ? EHookStrongWeakState::STRONG : EHookStrongWeakState::WEAK;
		Data.m_ShowHookStrongWeak = NameplateScopeAllowsPreview && g_Config.m_ClNamePlatesStrong > 0;

		// TClient
		Data.m_Local = false;
	};

	CNamePlateData Data;
	BuildPreviewData(Dummy, Data);

	CTeeRenderInfo TeeRenderInfo;
	if(Dummy == 0)
	{
		TeeRenderInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClPlayerSkin));
		TeeRenderInfo.ApplyColors(g_Config.m_ClPlayerUseCustomColor, g_Config.m_ClPlayerColorBody, g_Config.m_ClPlayerColorFeet);
	}
	else
	{
		TeeRenderInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClDummySkin));
		TeeRenderInfo.ApplyColors(g_Config.m_ClDummyUseCustomColor, g_Config.m_ClDummyColorBody, g_Config.m_ClDummyColorFeet);
	}
	TeeRenderInfo.m_Size = 64.0f;

	CNamePlate NamePlate(*GameClient(), Data);

	const bool FreeMoveEnabled = NameplateFreeMoveEnabled();
	// To keep the drag area visually identical when toggling between
	// player/dummy preview or nameplate visibility scopes, take the union of
	// both sides' full-scope baseline frame sizes and use that for
	// sizing/anchoring/clamping.
	CNamePlate FrameNamePlate;
	CNamePlate *pFrameNamePlate = nullptr;
	vec2 UnifiedFrameSize = NamePlate.Size();
	if(FreeMoveEnabled)
	{
		CNamePlateData FrameData;
		BuildPreviewData(Dummy, FrameData, true);
		FrameNamePlate.Update(*GameClient(), FrameData);
		pFrameNamePlate = &FrameNamePlate;

		CNamePlateData OtherFrameData;
		BuildPreviewData(Dummy == 0 ? 1 : 0, OtherFrameData, true);
		CNamePlate OtherFrameNamePlate(*GameClient(), OtherFrameData);
		UnifiedFrameSize = FrameNamePlate.Size();
		const vec2 OtherSize = OtherFrameNamePlate.Size();
		UnifiedFrameSize.x = std::max(UnifiedFrameSize.x, OtherSize.x);
		UnifiedFrameSize.y = std::max(UnifiedFrameSize.y, OtherSize.y);
		OtherFrameNamePlate.Reset(*GameClient());
	}

	Position.y += UnifiedFrameSize.y / 2.0f;
	Position.y += (float)g_Config.m_ClNamePlatesOffset / 2.0f;

	// Free-move preview: the tee stays at the original preview position (the
	// same relative offset to the nameplate content as in-game), so what the
	// user sees in the preview matches the actual in-game layout. The frame
	// (used for clamping the drag area) is still computed below, but it does
	// not move the tee.
	const vec2 TeeRenderPosition = Position;
	const vec2 NameplateBottomMiddle = Position - vec2(0.0f, (float)g_Config.m_ClNamePlatesOffset);
	bool HasFrame = false;
	vec2 FrameMin = vec2(0.0f, 0.0f);
	vec2 FrameMax = vec2(0.0f, 0.0f);
	if(FreeMoveEnabled)
	{
		// Anchor the unified frame at this preview's baseline-frame center,
		// then expand to the unified size and apply the visual scale.
		bool BaseHasFrame = false;
		vec2 BaseFrameMin = vec2(0.0f, 0.0f);
		vec2 BaseFrameMax = vec2(0.0f, 0.0f);
		pFrameNamePlate->ComputeBaselineFrame(NameplateBottomMiddle, BaseHasFrame, BaseFrameMin, BaseFrameMax);
		if(BaseHasFrame)
		{
			const vec2 Center = (BaseFrameMin + BaseFrameMax) * 0.5f;
			const vec2 HalfUnified = UnifiedFrameSize * 0.5f;
			FrameMin = Center - HalfUnified;
			FrameMax = Center + HalfUnified;
			ScaleFrameAroundCenter(FrameMin, FrameMax, NAMEPLATE_FREE_MOVE_FRAME_SCALE);
			HasFrame = true;
		}
	}

	// tee looking towards cursor, and it is happy when you touch it
	const vec2 DeltaPosition = Ui()->MousePos() - TeeRenderPosition;
	const float Distance = length(DeltaPosition);
	const float InteractionDistance = 20.0f;
	const vec2 TeeDirection = Distance < InteractionDistance ? normalize(vec2(DeltaPosition.x, maximum(DeltaPosition.y, 0.5f))) : normalize(DeltaPosition);
	const int TeeEmote = Distance < InteractionDistance ? EMOTE_HAPPY : (Dummy ? g_Config.m_ClDummyDefaultEyes : g_Config.m_ClPlayerDefaultEyes);
	RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeRenderInfo, TeeEmote, TeeDirection, TeeRenderPosition);
	Position.y -= (float)g_Config.m_ClNamePlatesOffset;
	if(FreeMoveEnabled)
	{
		std::array<SNameplateCoreRowRect, kNameplateCoreRowCount> aEditorRects;
		NamePlate.CollectCoreRowRects(Position, aEditorRects, pFrameNamePlate);

		const vec2 MousePosition = Ui()->MousePos();
		ENameplateCoreRow HoveredRow = ENameplateCoreRow::NUM_ROWS;
		for(const SNameplateCoreRowRect &Rect : aEditorRects)
		{
			if(NameplateCoreRowRectContains(Rect, MousePosition))
				HoveredRow = Rect.m_Row;
		}

		if(m_pData->m_FreeMoveDragRow != ENameplateCoreRow::NUM_ROWS && !Ui()->MouseButton(0))
		{
			m_pData->m_FreeMoveDragRow = ENameplateCoreRow::NUM_ROWS;
			m_pData->m_FreeMoveDragStartMouse = vec2(0.0f, 0.0f);
			m_pData->m_FreeMoveDragStartOffset = vec2(0.0f, 0.0f);
		}

		if(m_pData->m_FreeMoveDragRow == ENameplateCoreRow::NUM_ROWS && HoveredRow != ENameplateCoreRow::NUM_ROWS && Ui()->MouseButtonClicked(0) && Ui()->ActiveItem() == nullptr)
		{
			int *pOffsetX = NameplateCoreRowOffsetX(HoveredRow);
			int *pOffsetY = NameplateCoreRowOffsetY(HoveredRow);
			m_pData->m_FreeMoveDragRow = HoveredRow;
			m_pData->m_FreeMoveDragStartMouse = MousePosition;
			m_pData->m_FreeMoveDragStartOffset = vec2(pOffsetX != nullptr ? (float)*pOffsetX : 0.0f, pOffsetY != nullptr ? (float)*pOffsetY : 0.0f);
		}

		if(m_pData->m_FreeMoveDragRow != ENameplateCoreRow::NUM_ROWS && Ui()->MouseButton(0))
		{
			int *pOffsetX = NameplateCoreRowOffsetX(m_pData->m_FreeMoveDragRow);
			int *pOffsetY = NameplateCoreRowOffsetY(m_pData->m_FreeMoveDragRow);

			// Offset range = (FrameMin - BaselineRowMin, FrameMax - BaselineRowMax),
			// where BaselineRowMin/Max derive from row center & size at offset=0.
			// The frame here is the SAME unified+scaled frame the outline is
			// drawn from, so clamp matches the visible drag area exactly.
			bool DragHasRow = false;
			bool DragIgnoreFrame = false;
			vec2 DragIgnoreFrameMin = vec2(0.0f, 0.0f);
			vec2 DragIgnoreFrameMax = vec2(0.0f, 0.0f);
			vec2 DragRowCenter = vec2(0.0f, 0.0f);
			vec2 DragRowSize = vec2(0.0f, 0.0f);
			NamePlate.ComputeBaselineLayout(Position, m_pData->m_FreeMoveDragRow,
				DragIgnoreFrame, DragIgnoreFrameMin, DragIgnoreFrameMax,
				DragHasRow, DragRowCenter, DragRowSize, pFrameNamePlate);
			const vec2 DragFrameMin = FrameMin;
			const vec2 DragFrameMax = FrameMax;
			const bool DragHasFrame = HasFrame;

			int LimitMinX = NAMEPLATE_FREE_MOVE_OFFSET_MIN;
			int LimitMaxX = NAMEPLATE_FREE_MOVE_OFFSET_MAX;
			int LimitMinY = NAMEPLATE_FREE_MOVE_OFFSET_MIN;
			int LimitMaxY = NAMEPLATE_FREE_MOVE_OFFSET_MAX;
			if(DragHasFrame && DragHasRow)
			{
				const vec2 BaselineRowMin = vec2(DragRowCenter.x - DragRowSize.x / 2.0f, DragRowCenter.y - DragRowSize.y / 2.0f);
				const vec2 BaselineRowMax = vec2(DragRowCenter.x + DragRowSize.x / 2.0f, DragRowCenter.y + DragRowSize.y / 2.0f);
				const float MinX = DragFrameMin.x - BaselineRowMin.x;
				const float MaxX = DragFrameMax.x - BaselineRowMax.x;
				const float MinY = DragFrameMin.y - BaselineRowMin.y;
				const float MaxY = DragFrameMax.y - BaselineRowMax.y;
				if(MinX <= MaxX)
				{
					LimitMinX = std::max(LimitMinX, round_to_int(MinX));
					LimitMaxX = std::min(LimitMaxX, round_to_int(MaxX));
				}
				else
				{
					LimitMinX = 0;
					LimitMaxX = 0;
				}
				if(MinY <= MaxY)
				{
					LimitMinY = std::max(LimitMinY, round_to_int(MinY));
					LimitMaxY = std::min(LimitMaxY, round_to_int(MaxY));
				}
				else
				{
					LimitMinY = 0;
					LimitMaxY = 0;
				}
			}

			if(pOffsetX != nullptr)
			{
				int NewOffsetX = std::clamp(round_to_int(m_pData->m_FreeMoveDragStartOffset.x + MousePosition.x - m_pData->m_FreeMoveDragStartMouse.x), LimitMinX, LimitMaxX);
				if(DragHasFrame && DragHasRow)
					NewOffsetX = NameplateSnapCoreRowOffsetX(NewOffsetX, LimitMinX, LimitMaxX, DragRowCenter, DragRowSize, DragFrameMin, DragFrameMax);
				*pOffsetX = NewOffsetX;
			}
			if(pOffsetY != nullptr)
			{
				const int NewOffsetY = std::clamp(round_to_int(m_pData->m_FreeMoveDragStartOffset.y + MousePosition.y - m_pData->m_FreeMoveDragStartMouse.y), LimitMinY, LimitMaxY);
				*pOffsetY = NewOffsetY;
			}
			NamePlate.CollectCoreRowRects(Position, aEditorRects, pFrameNamePlate);
		}

		const bool DraggingAnyRow = m_pData->m_FreeMoveDragRow != ENameplateCoreRow::NUM_ROWS;
		const bool HoveringFrame = HasFrame && NameplatePointInFrame(MousePosition, FrameMin, FrameMax);
		const bool ShowEditorFrame = DraggingAnyRow || HoveringFrame;
		if(ShowEditorFrame)
		{
			Graphics()->TextureClear();
			if(HasFrame)
			{
				Graphics()->SetColor(ColorRGBA(0.35f, 0.75f, 1.0f, 0.38f));
				Graphics()->LinesBegin();
				const IGraphics::CLineItem aFrameLines[] = {
					IGraphics::CLineItem(FrameMin.x, FrameMin.y, FrameMax.x, FrameMin.y),
					IGraphics::CLineItem(FrameMax.x, FrameMin.y, FrameMax.x, FrameMax.y),
					IGraphics::CLineItem(FrameMax.x, FrameMax.y, FrameMin.x, FrameMax.y),
					IGraphics::CLineItem(FrameMin.x, FrameMax.y, FrameMin.x, FrameMin.y),
				};
				Graphics()->LinesDraw(aFrameLines, std::size(aFrameLines));
				Graphics()->LinesEnd();
			}

			Graphics()->QuadsBegin();
			for(const SNameplateCoreRowRect &Rect : aEditorRects)
			{
				if(!Rect.m_Visible)
					continue;
				const bool Dragging = m_pData->m_FreeMoveDragRow == Rect.m_Row;
				const bool Hovered = HoveredRow == Rect.m_Row;
				if(Dragging)
					Graphics()->SetColor(ColorRGBA(0.25f, 0.85f, 1.0f, 0.22f));
				else if(Hovered)
					Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.14f));
				else
					Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.06f));
				Graphics()->DrawRectExt(Rect.m_Min.x, Rect.m_Min.y, Rect.m_Max.x - Rect.m_Min.x, Rect.m_Max.y - Rect.m_Min.y, 4.0f, IGraphics::CORNER_ALL);
			}
			Graphics()->QuadsEnd();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
	}
	else
	{
		m_pData->m_FreeMoveDragRow = ENameplateCoreRow::NUM_ROWS;
	}
	NamePlate.Render(*GameClient(), Position, pFrameNamePlate);
	NamePlate.Reset(*GameClient());
	if(pFrameNamePlate != nullptr)
		pFrameNamePlate->Reset(*GameClient());
}

void CNamePlates::ResetNamePlates()
{
	for(CNamePlate &NamePlate : m_pData->m_aNamePlates)
		NamePlate.Reset(*GameClient());
	for(CNamePlate &NamePlate : m_pData->m_aNamePlateFrameReferences)
		NamePlate.Reset(*GameClient());
	for(SCoordXAlignState &CoordXAlignState : m_pData->m_aCoordXAlign)
		CoordXAlignState = SCoordXAlignState();
}

void CNamePlates::ResetChatBubbleAnimState(int ClientId, bool IsDestructing)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;

	SChatBubbleAnimState &AnimState = m_pData->m_aChatBubbleAnim[ClientId];
	if(IsDestructing)
	{
		TextRender()->DeleteTextContainer(AnimState.m_TextContainerIndex);
		AnimState = SChatBubbleAnimState();
		return;
	}
	CUiV2AnimationRuntime &AnimRuntime = GameClient()->UiRuntimeV2()->AnimRuntime();
	const uint64_t NodeKey = ChatBubbleAnimNodeKey(ClientId);
	const bool RuntimeInitialized = AnimRuntime.GetValue(NodeKey, EUiAnimProperty::ALPHA, -1.0f) >= -0.5f;
	if(!AnimState.m_Initialized && AnimState.m_aCachedText[0] == '\0' && !RuntimeInitialized)
		return;

	TextRender()->DeleteTextContainer(AnimState.m_TextContainerIndex);
	AnimState = SChatBubbleAnimState();
	AnimRuntime.SetValue(NodeKey, EUiAnimProperty::ALPHA, 0.0f);
	AnimRuntime.SetValue(NodeKey, EUiAnimProperty::SCALE, 1.0f);
	AnimRuntime.SetValue(NodeKey, EUiAnimProperty::POS_Y, 0.0f);
	AnimRuntime.SetValue(NodeKey, EUiAnimProperty::WIDTH, 0.0f);
}

void CNamePlates::RenderChatBubble(vec2 Position, int ClientId, float Alpha)
{
	// Check if chat bubbles are enabled
	if(!g_Config.m_QmChatBubble)
		return;
	if(FocusModeHidesChat())
		return;

	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;

	auto &ClientData = GameClient()->m_aClients[ClientId];
	CUiV2AnimationRuntime &AnimRuntime = GameClient()->UiRuntimeV2()->AnimRuntime();
	SChatBubbleAnimState &AnimState = m_pData->m_aChatBubbleAnim[ClientId];
	const uint64_t NodeKey = ChatBubbleAnimNodeKey(ClientId);
	const bool RuntimeNeedsInit = AnimRuntime.GetValue(NodeKey, EUiAnimProperty::ALPHA, -1.0f) < -0.5f;
	if(!AnimState.m_Initialized || RuntimeNeedsInit)
	{
		TextRender()->DeleteTextContainer(AnimState.m_TextContainerIndex);
		AnimState = SChatBubbleAnimState();
		AnimState.m_Initialized = true;
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::ALPHA, 0.0f);
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::SCALE, 1.0f);
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::POS_Y, 0.0f);
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::WIDTH, 0.0f);
	}

	// Determine what text to show
	const char *pBubbleText = nullptr;
	bool IsTyping = false;
	int64_t Now = time();

	// Check if this is local player who is currently typing
	const bool IsLocalPlayer = (GameClient()->m_Snap.m_LocalClientId == ClientId);
	if(IsLocalPlayer && GameClient()->m_Chat.IsActive())
	{
		const char *pInputText = GameClient()->m_Chat.GetInputText();
		if(pInputText && pInputText[0] != '\0')
		{
			pBubbleText = pInputText;
			IsTyping = true;
		}
	}

	// If not typing, check for existing chat bubble
	if(!pBubbleText)
	{
		if(ClientData.m_aChatBubbleText[0] != '\0' && Now < ClientData.m_ChatBubbleExpireTick)
			pBubbleText = ClientData.m_aChatBubbleText;
	}

	const bool HasSourceText = pBubbleText && pBubbleText[0] != '\0';
	if(HasSourceText)
		str_copy(AnimState.m_aCachedText, pBubbleText, sizeof(AnimState.m_aCachedText));

	if(!HasSourceText && AnimState.m_aCachedText[0] == '\0')
	{
		AnimState.m_LastVisible = false;
		AnimState.m_LastTyping = false;
		return;
	}

	const bool TimedDisappearWindow = HasSourceText && !IsTyping && (ClientData.m_ChatBubbleExpireTick - Now <= time_freq());
	const bool NewTimedBubble = HasSourceText && !IsTyping && ClientData.m_ChatBubbleStartTick != AnimState.m_LastChatBubbleStartTick;
	if(NewTimedBubble)
		AnimState.m_LastChatBubbleStartTick = ClientData.m_ChatBubbleStartTick;

	const bool BecameVisible = HasSourceText && !AnimState.m_LastVisible;
	if(BecameVisible || NewTimedBubble)
	{
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::ALPHA, 0.0f);
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::SCALE, 0.92f);
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::POS_Y, 12.0f);
		if(IsTyping)
			AnimRuntime.SetValue(NodeKey, EUiAnimProperty::WIDTH, 0.0f);
		AnimState.m_LastTargetAlpha = std::numeric_limits<float>::quiet_NaN();
		AnimState.m_LastTargetScale = std::numeric_limits<float>::quiet_NaN();
		AnimState.m_LastTargetSlide = std::numeric_limits<float>::quiet_NaN();
		AnimState.m_LastTargetFillChars = std::numeric_limits<float>::quiet_NaN();
	}

	float TargetAlpha = HasSourceText ? Alpha : 0.0f;
	float TargetScale = 1.0f;
	float TargetSlideOffset = 0.0f;
	float AlphaDuration = HasSourceText ? 0.20f : 0.16f;
	float TransformDuration = HasSourceText ? 0.20f : 0.16f;

	if(TimedDisappearWindow)
	{
		TargetAlpha = 0.0f;
		AlphaDuration = 1.0f;
		TransformDuration = 1.0f;
		int AnimationType = g_Config.m_QmChatBubbleAnimation;
		switch(AnimationType)
		{
		case 1:
			TargetScale = 0.2f;
			break;
		case 2:
			TargetSlideOffset = 50.0f;
			break;
		case 0:
		default:
			break;
		}
	}

	const float BubbleAlpha = std::clamp(ResolveChatBubbleAnimValue(AnimRuntime, NodeKey, EUiAnimProperty::ALPHA, TargetAlpha, AlphaDuration, EEasing::EASE_OUT, AnimState.m_LastTargetAlpha), 0.0f, Alpha);
	const float AnimScale = std::max(0.0f, ResolveChatBubbleAnimValue(AnimRuntime, NodeKey, EUiAnimProperty::SCALE, TargetScale, TransformDuration, EEasing::EASE_OUT, AnimState.m_LastTargetScale));
	const float AnimSlideOffset = ResolveChatBubbleAnimValue(AnimRuntime, NodeKey, EUiAnimProperty::POS_Y, TargetSlideOffset, TransformDuration, EEasing::EASE_OUT, AnimState.m_LastTargetSlide);

	AnimState.m_LastVisible = HasSourceText;
	AnimState.m_LastTyping = IsTyping;

	const char *pRenderText = HasSourceText ? pBubbleText : AnimState.m_aCachedText;
	char aAnimatedText[sizeof(AnimState.m_aCachedText)];
	aAnimatedText[0] = '\0';
	const char *pDisplayText = pRenderText;

	size_t RenderBytes = 0;
	size_t RenderCharCountSize = 0;
	str_utf8_stats(pRenderText, std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), &RenderBytes, &RenderCharCountSize);
	(void)RenderBytes;
	const int RenderCharCount = static_cast<int>(RenderCharCountSize);

	if(IsTyping)
	{
		const float TargetFillChars = static_cast<float>(RenderCharCount);
		const float CurrentFillChars = AnimRuntime.GetValue(NodeKey, EUiAnimProperty::WIDTH, TargetFillChars);
		const float DeltaChars = std::abs(TargetFillChars - CurrentFillChars);
		float FillDuration = std::clamp(DeltaChars * 0.035f, 0.05f, 0.25f);
		if(TargetFillChars < CurrentFillChars)
			FillDuration = 0.08f;
		const float AnimatedFillChars = ResolveChatBubbleAnimValue(AnimRuntime, NodeKey, EUiAnimProperty::WIDTH, TargetFillChars, FillDuration, EEasing::EASE_OUT, AnimState.m_LastTargetFillChars);
		const int VisibleChars = std::clamp(round_to_int(AnimatedFillChars), 0, RenderCharCount);
		str_utf8_copy_num(aAnimatedText, pRenderText, sizeof(aAnimatedText), VisibleChars);
		pDisplayText = aAnimatedText;
	}
	else
	{
		AnimRuntime.SetValue(NodeKey, EUiAnimProperty::WIDTH, static_cast<float>(RenderCharCount));
		AnimState.m_LastTargetFillChars = std::numeric_limits<float>::quiet_NaN();
	}

	if(BubbleAlpha <= 0.001f)
	{
		if(!HasSourceText)
			AnimState.m_aCachedText[0] = '\0';
		return;
	}

	// Get current screen bounds (world coordinates with current zoom)
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	// Check if position is on screen
	if(!(in_range(Position.x, ScreenX0 - 400, ScreenX1 + 400) && in_range(Position.y, ScreenY0 - 400, ScreenY1 + 400)))
		return;

	// Calculate screen-space position (0-1 range) for zoom independence
	float ScreenWidth = ScreenX1 - ScreenX0;
	float ScreenHeight = ScreenY1 - ScreenY0;
	float ScreenPosX = (Position.x - ScreenX0) / ScreenWidth;

	// Map to interface coordinates for text rendering (fixed zoom)
	Graphics()->MapScreenToInterface(GameClient()->m_Camera.m_Center.x, GameClient()->m_Camera.m_Center.y);

	// Get interface screen bounds (fixed regardless of zoom)
	float InterfaceX0, InterfaceY0, InterfaceX1, InterfaceY1;
	Graphics()->GetScreen(&InterfaceX0, &InterfaceY0, &InterfaceX1, &InterfaceY1);
	float InterfaceWidth = InterfaceX1 - InterfaceX0;
	float InterfaceHeight = InterfaceY1 - InterfaceY0;

	// Convert screen-space position to interface coordinates
	float InterfaceX = InterfaceX0 + ScreenPosX * InterfaceWidth;

	// Keep bubble sizing in screen space so rapid camera zoom does not force
	// a full text relayout every frame.
	constexpr float kBubblePadding = 12.0f;
	constexpr float kBubbleRounding = 10.0f;
	constexpr float kBubbleMaxWidth = 230.0f;
	const float BaseFontSize = (float)g_Config.m_QmChatBubbleFontSize;

	const float FontSize = BaseFontSize;
	const float Padding = kBubblePadding;
	const float Rounding = kBubbleRounding;
	const float MaxWidth = kBubbleMaxWidth;

	// Anchor bubble to the top of the nameplate plus default nameplate spacing.
	float NameplateTopWorldY = Position.y - (float)g_Config.m_ClNamePlatesOffset;
	const vec2 NameplateBottomMiddleWorld = Position - vec2(0.0f, (float)g_Config.m_ClNamePlatesOffset);
	const CNamePlate &NamePlate = m_pData->m_aNamePlates[ClientId];
	if(NamePlate.IsInitialized())
		NameplateTopWorldY = NamePlate.TopY(NameplateBottomMiddleWorld);
	const float ScreenPosNameplateTopY = (NameplateTopWorldY - ScreenY0) / ScreenHeight;
	const float InterfaceNameplateTopY = InterfaceY0 + ScreenPosNameplateTopY * InterfaceHeight;

	unsigned int PrevFlags = TextRender()->GetRenderFlags();
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT);

	const bool UseTextContainer = !IsTyping && std::abs(AnimScale - 1.0f) <= 0.001f;
	const bool LayoutDirty = !AnimState.m_TextContainerIndex.Valid() ||
				 str_comp(AnimState.m_aLayoutText, pDisplayText) != 0 ||
				 AnimState.m_CachedFontSize != FontSize ||
				 AnimState.m_CachedLineWidth != MaxWidth;

	if(!IsTyping && LayoutDirty)
	{
		TextRender()->DeleteTextContainer(AnimState.m_TextContainerIndex);

		CTextCursor LayoutCursor;
		LayoutCursor.m_FontSize = FontSize;
		LayoutCursor.m_LineWidth = MaxWidth;
		if(TextRender()->CreateTextContainer(AnimState.m_TextContainerIndex, &LayoutCursor, pDisplayText))
		{
			const STextBoundingBox BoundingBox = TextRender()->GetBoundingBoxTextContainer(AnimState.m_TextContainerIndex);
			AnimState.m_CachedTextWidth = BoundingBox.m_W;
			AnimState.m_CachedTextHeight = BoundingBox.m_H;
			AnimState.m_CachedFontSize = FontSize;
			AnimState.m_CachedLineWidth = MaxWidth;
			str_copy(AnimState.m_aLayoutText, pDisplayText, sizeof(AnimState.m_aLayoutText));
		}
		else
		{
			AnimState.m_CachedTextWidth = 0.0f;
			AnimState.m_CachedTextHeight = 0.0f;
			AnimState.m_CachedFontSize = FontSize;
			AnimState.m_CachedLineWidth = MaxWidth;
			AnimState.m_aLayoutText[0] = '\0';
		}
	}

	float TextHeight = 0.0f;
	float TextWidth = 0.0f;
	if(IsTyping)
	{
		CTextCursor MeasureCursor;
		MeasureCursor.m_FontSize = FontSize;
		MeasureCursor.m_LineWidth = MaxWidth;
		MeasureCursor.m_Flags = 0;
		TextRender()->TextEx(&MeasureCursor, pDisplayText);
		TextHeight = MeasureCursor.Height();
		TextWidth = MeasureCursor.m_LongestLineWidth;
	}
	else
	{
		TextHeight = AnimState.m_CachedTextHeight;
		TextWidth = AnimState.m_CachedTextWidth;
	}

	// Calculate bubble dimensions and position (with scale animation)
	float BubbleWidth = (TextWidth + Padding * 2.0f) * AnimScale;
	float BubbleHeight = (TextHeight + Padding * 2.0f) * AnimScale;

	// Position bubble above the nameplate with built-in nameplate spacing.
	float AnimOffset = AnimSlideOffset;
	const float BubbleGap = DEFAULT_PADDING;
	float BubbleX = InterfaceX - BubbleWidth / 2.0f;
	float BubbleY = InterfaceNameplateTopY - BubbleGap - BubbleHeight + AnimOffset;

	float ConfigAlpha = g_Config.m_QmChatBubbleAlpha / 100.0f;

	ColorHSLA BgHSLA(g_Config.m_QmChatBubbleBgColor, true);
	ColorRGBA BgColor = color_cast<ColorRGBA>(BgHSLA);
	BgColor.a *= BubbleAlpha * ConfigAlpha;

	Graphics()->TextureClear();
	Graphics()->DrawRect(
		BubbleX, BubbleY,
		BubbleWidth, BubbleHeight,
		BgColor,
		IGraphics::CORNER_ALL,
		Rounding);

	ColorHSLA TextHSLA(g_Config.m_QmChatBubbleTextColor, false);
	ColorRGBA TextColor = color_cast<ColorRGBA>(TextHSLA);
	TextColor.a *= BubbleAlpha * ConfigAlpha;

	ColorRGBA OutlineColor = TextRender()->DefaultTextOutlineColor();
	OutlineColor.a *= TextColor.a;

	TextRender()->TextColor(TextColor);
	TextRender()->TextOutlineColor(OutlineColor);

	if(UseTextContainer && AnimState.m_TextContainerIndex.Valid())
	{
		const float TextX = BubbleX + (BubbleWidth - TextWidth) / 2.0f;
		const float TextY = BubbleY + (BubbleHeight - TextHeight) / 2.0f;
		TextRender()->RenderTextContainer(AnimState.m_TextContainerIndex, TextColor, OutlineColor, TextX, TextY);
	}
	else
	{
		const float ScaledTextWidth = TextWidth * AnimScale;
		const float ScaledTextHeight = TextHeight * AnimScale;
		const float TextX = BubbleX + (BubbleWidth - ScaledTextWidth) / 2.0f;
		const float TextY = BubbleY + (BubbleHeight - ScaledTextHeight) / 2.0f;

		CTextCursor Cursor;
		Cursor.m_FontSize = FontSize * AnimScale;
		Cursor.m_LineWidth = MaxWidth * AnimScale;
		Cursor.m_Flags = TEXTFLAG_RENDER;
		Cursor.SetPosition(vec2(TextX, TextY));
		TextRender()->TextEx(&Cursor, pDisplayText);
	}

	TextRender()->TextColor(TextRender()->DefaultTextColor());
	TextRender()->TextOutlineColor(TextRender()->DefaultTextOutlineColor());
	TextRender()->SetRenderFlags(PrevFlags);

	Graphics()->MapScreen(ScreenX0, ScreenY0, ScreenX1, ScreenY1);
}

void CNamePlates::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	int ShowDirection = g_Config.m_ClShowDirection;
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current())
		ShowDirection = g_Config.m_ClVideoShowDirection;
#endif
	const bool ShowCoordXAlignHint = g_Config.m_QmNameplateCoordXAlignHint || g_Config.m_QmNameplateCoordXAlignHintStrict;
	const bool ShowCoords = (g_Config.m_QmNameplateCoords || g_Config.m_QmNameplateCoordsOwn) &&
				(g_Config.m_QmNameplateCoordX || g_Config.m_QmNameplateCoordY);
	const bool RenderNames = g_Config.m_ClNamePlates || g_Config.m_ClNamePlatesOwn;
	const bool RenderClan = g_Config.m_ClNamePlatesClan || (g_Config.m_TcWarList && g_Config.m_TcWarListShowClan);
	const bool RenderClientIds = g_Config.m_Debug || g_Config.m_ClNamePlatesIds;
	const bool RenderStrongWeak = g_Config.m_Debug || g_Config.m_ClNamePlatesStrong > 0;
	const bool RenderTClientExtras = g_Config.m_TcNameplatePingCircle || g_Config.m_TcNameplateCountry || g_Config.m_TcNameplateSkins || (g_Config.m_TcWarList && g_Config.m_TcWarListReason);
	const bool RenderDirection = ShowDirection != 0;
	const bool RenderNameplates = RenderNames || RenderClan || RenderClientIds || RenderStrongWeak || RenderTClientExtras || RenderDirection || ShowCoords || ShowCoordXAlignHint;
	const bool RenderChatBubbles = g_Config.m_QmChatBubble != 0 && !FocusModeHidesChat();
	const bool RenderFreezeWakeupPopups = GameClient()->HasFreezeWakeupPopups();
	if(!RenderNameplates && !RenderChatBubbles && !RenderFreezeWakeupPopups)
		return;

	if(RenderNameplates || RenderChatBubbles)
	{
		if(RenderNameplates)
			UpdateCoordXAlignFrameState();

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[i];
			if(!pInfo)
			{
				ResetChatBubbleAnimState(i);
				m_pData->m_aCoordXAlign[i] = SCoordXAlignState();
				continue;
			}

			// Each player can also have a spectator char whose name plate is displayed independently
			const bool FollowedCharacterWillRender =
				GameClient()->m_Snap.m_SpecInfo.m_Active &&
				!GameClient()->m_MultiViewActivated &&
				GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == i &&
				GameClient()->m_Snap.m_aCharacters[i].m_Active;
			if(GameClient()->m_aClients[i].m_SpecCharPresent && RenderNameplates && !FollowedCharacterWillRender)
			{
				const vec2 RenderPos = GameClient()->m_aClients[i].m_SpecChar;
				RenderNamePlateGame(RenderPos, pInfo, 0.4f, false);
			}
			// Only render name plates for active characters
			if(GameClient()->m_Snap.m_aCharacters[i].m_Active)
			{
				// TClient
				if(GameClient()->m_aClients[i].m_IsVolleyBall)
					continue;
				// if(g_Config.m_TcRenderNameplateSpec > 0)
				//	continue;
				const vec2 RenderPos = GameClient()->m_aClients[i].m_RenderPos;
				if(RenderNameplates || RenderChatBubbles)
					RenderNamePlateGame(RenderPos, pInfo, RenderNameplates ? 1.0f : 0.0f);

				// Render chat bubble above player
				if(RenderChatBubbles)
					RenderChatBubble(RenderPos, i, 1.0f);
			}
		}
	}

	if(RenderFreezeWakeupPopups)
		GameClient()->RenderFreezeWakeupPopups();
}

void CNamePlates::OnWindowResize()
{
	ResetNamePlates();
	for(int i = 0; i < MAX_CLIENTS; ++i)
		ResetChatBubbleAnimState(i);
}

CNamePlates::CNamePlates() :
	m_pData(new CNamePlates::CNamePlatesData()) {}

CNamePlates::~CNamePlates()
{
	delete m_pData;
}
