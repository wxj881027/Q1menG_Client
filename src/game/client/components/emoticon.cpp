/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "emoticon.h"

#include "chat.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>

CEmoticon::CEmoticon()
{
	OnReset();
}

void CEmoticon::ConKeyEmoticon(IConsole::IResult *pResult, void *pUserData)
{
	CEmoticon *pSelf = (CEmoticon *)pUserData;

	if(pSelf->GameClient()->m_Scoreboard.IsActive())
		return;

	if(!pSelf->GameClient()->m_Snap.m_SpecInfo.m_Active && pSelf->Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(pSelf->GameClient()->m_BindWheel.IsActive())
			pSelf->m_Active = false;
		else
			pSelf->m_Active = pResult->GetInteger(0) != 0;
	}
}

void CEmoticon::ConEmote(IConsole::IResult *pResult, void *pUserData)
{
	((CEmoticon *)pUserData)->Emote(pResult->GetInteger(0));
}

void CEmoticon::OnConsoleInit()
{
	Console()->Register("+emote", "", CFGFLAG_CLIENT, ConKeyEmoticon, this, "Open emote selector");
	Console()->Register("emote", "i[emote-id]", CFGFLAG_CLIENT, ConEmote, this, "Use emote");
}

void CEmoticon::OnReset()
{
	m_WasActive = false;
	m_Active = false;
	m_SelectedEmote = -1;
	m_SelectedEyeEmote = -1;
	m_TouchPressedOutside = false;
}

void CEmoticon::OnRelease()
{
	m_Active = false;
}

bool CEmoticon::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!m_Active)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	m_SelectorMouse += vec2(x, y);
	return true;
}

bool CEmoticon::OnInput(const IInput::CEvent &Event)
{
	if(IsActive() && Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		OnRelease();
		return true;
	}
	return false;
}

void CEmoticon::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	// QuadEaseInOut - 原始平滑缓动
	static const auto QuadEaseInOut = [](float t) -> float {
		if(t == 0.0f)
			return 0.0f;
		if(t == 1.0f)
			return 1.0f;
		return (t < 0.5f) ? (2.0f * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 2) / 2.0f);
	};
	
	// EaseOutBack - 弹出时有过冲效果，更有弹性
	static const auto EaseOutBack = [](float t) -> float {
		if(t == 0.0f)
			return 0.0f;
		if(t == 1.0f)
			return 1.0f;
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		return 1.0f + c3 * std::pow(t - 1.0f, 3) + c1 * std::pow(t - 1.0f, 2);
	};
	
	// EaseInBack - 收回时有回弹效果
	static const auto EaseInBack = [](float t) -> float {
		if(t == 0.0f)
			return 0.0f;
		if(t == 1.0f)
			return 1.0f;
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		return c3 * t * t * t - c1 * t * t;
	};
	
	// EaseOutElastic - 橡皮筋弹跳效果（更夸张）
	static const auto EaseOutElastic = [](float t) -> float {
		if(t == 0.0f)
			return 0.0f;
		if(t == 1.0f)
			return 1.0f;
		const float c4 = (2.0f * pi) / 3.0f;
		return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
	};
	
	static const auto PositiveMod = [](float x, float y) -> float {
		return std::fmod(x + y, y);
	};

	static const float s_InnerMouseLimitRadius = 40.0f;
	static const float s_InnerOuterMouseBoundaryRadius = 110.0f;
	static const float s_OuterMouseLimitRadius = 170.0f;
	static const float s_InnerItemRadius = 70.0f;
	static const float s_OuterItemRadius = 150.0f;
	static const float s_InnerCircleRadius = 100.0f;
	static const float s_OuterCircleRadius = 190.0f;

	// 动画时间设置 - 基础动画时间
	const float BaseAnimationTime = (float)g_Config.m_TcAnimateWheelTime / 1000.0f;
	// 实际动画时间（考虑分层延迟）
	const float AnimationTime = BaseAnimationTime * 1.2f; // 增加20%时间让动画更流畅
	const float ItemAnimationTime = AnimationTime / 2.0f;
	const float StaggerDelay = AnimationTime * 0.015f; // 减小延迟比例，让图标更紧凑地弹出

	if(AnimationTime != 0.0f)
	{
		for(float &Time : m_aAnimationTimeEmotes)
			Time = std::max(0.0f, Time - Client()->RenderFrameTime());
		for(float &Time : m_aAnimationTimeEyeEmotes)
			Time = std::max(0.0f, Time - Client()->RenderFrameTime());
	}

	if(!m_Active)
	{
		if(m_TouchPressedOutside)
		{
			m_SelectedEmote = -1;
			m_SelectedEyeEmote = -1;
			m_TouchPressedOutside = false;
		}

		if(m_WasActive && m_SelectedEmote != -1)
			Emote(m_SelectedEmote);
		if(m_WasActive && m_SelectedEyeEmote != -1)
			EyeEmote(m_SelectedEyeEmote);
		m_WasActive = false;

		if(AnimationTime == 0.0f)
			return;

		// 关闭动画速度与打开速度相同，让效果对称
		m_AnimationTime -= Client()->RenderFrameTime() * 1.2f;
		if(m_AnimationTime <= 0.0f)
		{
			m_AnimationTime = 0.0f;
			return;
		}
	}
	else
	{
		if(AnimationTime != 0.0f)
		{
			m_AnimationTime += Client()->RenderFrameTime();
			if(m_AnimationTime > AnimationTime)
				m_AnimationTime = AnimationTime;
		}
		m_WasActive = true;
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		m_Active = false;
		m_WasActive = false;
	}

	const CUIRect Screen = *Ui()->Screen();

	const bool WasTouchPressed = m_TouchState.m_AnyPressed;
	Ui()->UpdateTouchState(m_TouchState);
	if(m_TouchState.m_AnyPressed)
	{
		const vec2 TouchPos = (m_TouchState.m_PrimaryPosition - vec2(0.5f, 0.5f)) * Screen.Size();
		const float TouchCenterDistance = length(TouchPos);
		if(TouchCenterDistance <= s_OuterMouseLimitRadius)
		{
			m_SelectorMouse = TouchPos;
		}
		else if(TouchCenterDistance > s_OuterCircleRadius)
		{
			m_TouchPressedOutside = true;
		}
	}
	else if(WasTouchPressed)
	{
		m_Active = false;
	}

	std::array<float, 5> aAnimationPhase;
	std::array<float, 5> aAlphaPhase; // 透明度动画分层
	
	// 判断是打开还是关闭动画
	const bool IsOpening = m_Active;
	
	if(AnimationTime == 0.0f)
	{
		aAnimationPhase.fill(1.0f);
		aAlphaPhase.fill(1.0f);
	}
	else
	{
		const float NormalizedTime = m_AnimationTime / AnimationTime;
		
		if(IsOpening)
		{
			// 打开动画使用弹性缓动，让图标有弹出感
			aAnimationPhase[0] = EaseOutBack(NormalizedTime);
			// 透明度使用较快的线性渐变
			aAlphaPhase[0] = std::min(1.0f, NormalizedTime * 1.5f);
		}
		else
		{
			// 关闭动画使用EaseInBack，让图标收回时有吸入感
			aAnimationPhase[0] = EaseInBack(1.0f - (1.0f - NormalizedTime));
			// 透明度提前开始淡出
			aAlphaPhase[0] = NormalizedTime;
		}
		
		// 分层效果 - 使用不同的缓动强度
		aAnimationPhase[1] = aAnimationPhase[0] * QuadEaseInOut(NormalizedTime);
		aAnimationPhase[2] = aAnimationPhase[1] * aAnimationPhase[0];
		aAnimationPhase[3] = aAnimationPhase[2] * QuadEaseInOut(NormalizedTime);
		aAnimationPhase[4] = aAnimationPhase[3] * aAnimationPhase[0];
		
		// 透明度分层
		aAlphaPhase[1] = aAlphaPhase[0] * aAlphaPhase[0];
		aAlphaPhase[2] = aAlphaPhase[1] * aAlphaPhase[0];
		aAlphaPhase[3] = aAlphaPhase[2] * aAlphaPhase[0];
		aAlphaPhase[4] = aAlphaPhase[3] * aAlphaPhase[0];
	}

	if(length(m_SelectorMouse) > s_OuterMouseLimitRadius)
		m_SelectorMouse = normalize(m_SelectorMouse) * s_OuterMouseLimitRadius;

	const float SelectorAngle = angle(m_SelectorMouse);

	m_SelectedEmote = -1;
	m_SelectedEyeEmote = -1;
	if(length(m_SelectorMouse) > s_InnerOuterMouseBoundaryRadius)
		m_SelectedEmote = PositiveMod(std::round(SelectorAngle / (2.0f * pi) * NUM_EMOTICONS), NUM_EMOTICONS);
	else if(length(m_SelectorMouse) > s_InnerMouseLimitRadius)
		m_SelectedEyeEmote = PositiveMod(std::round(SelectorAngle / (2.0f * pi) * NUM_EMOTES), NUM_EMOTES);

	if(m_SelectedEmote != -1)
	{
		m_aAnimationTimeEmotes[m_SelectedEmote] += Client()->RenderFrameTime() * 2.0f; // To counteract earlier decrement
		if(m_aAnimationTimeEmotes[m_SelectedEmote] >= ItemAnimationTime)
			m_aAnimationTimeEmotes[m_SelectedEmote] = ItemAnimationTime;
	}
	if(m_SelectedEyeEmote != -1)
	{
		m_aAnimationTimeEyeEmotes[m_SelectedEyeEmote] += Client()->RenderFrameTime() * 2.0f; // To counteract earlier decrement
		if(m_aAnimationTimeEyeEmotes[m_SelectedEyeEmote] >= ItemAnimationTime)
			m_aAnimationTimeEyeEmotes[m_SelectedEyeEmote] = ItemAnimationTime;
	}

	const vec2 ScreenCenter = Screen.Center();

	Ui()->MapScreen();

	Graphics()->BlendNormal();

	// 外圈背景 - 使用弹性缩放和透明度渐变
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.3f * aAlphaPhase[0]);
	Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, s_OuterCircleRadius * aAnimationPhase[0], 64);
	Graphics()->QuadsEnd();

	Graphics()->WrapClamp();
	for(int Emote = 0; Emote < NUM_EMOTICONS; Emote++)
	{
		float Angle = 2.0f * pi * Emote / NUM_EMOTICONS;
		if(Angle > pi)
			Angle -= 2.0f * pi;

		// 计算每个图标的延迟弹出时间
		const float IconDelay = StaggerDelay * (float)Emote;
		float IconAnimTime = m_AnimationTime;
		
		if(IsOpening)
		{
			// 打开时，按顺序延迟弹出
			IconAnimTime = std::max(0.0f, m_AnimationTime - IconDelay);
		}
		else
		{
			// 关闭时，逆序收回（最后弹出的先收回）
			IconAnimTime = std::max(0.0f, m_AnimationTime - StaggerDelay * (float)(NUM_EMOTICONS - 1 - Emote));
		}
		
		const float IconNormalizedTime = std::min(1.0f, IconAnimTime / (AnimationTime - StaggerDelay * (NUM_EMOTICONS - 1)));
		
		// 每个图标有独立的弹性动画
		float IconPhase;
		float IconAlpha;
		if(AnimationTime == 0.0f)
		{
			IconPhase = 1.0f;
			IconAlpha = 1.0f;
		}
		else if(IsOpening)
		{
			IconPhase = EaseOutBack(IconNormalizedTime);
			IconAlpha = std::min(1.0f, IconNormalizedTime * 2.0f);
		}
		else
		{
			IconPhase = EaseInBack(IconNormalizedTime);
			IconAlpha = IconNormalizedTime;
		}

		Graphics()->TextureSet(GameClient()->m_EmoticonsSkin.m_aSpriteEmoticons[Emote]);
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadsBegin();
		
		// 计算位置和大小
		const vec2 Nudge = direction(Angle) * s_OuterItemRadius * IconPhase;
		const float HoverPhase = ItemAnimationTime == 0.0f ? (Emote == m_SelectedEmote ? 1.0f : 0.0f) : EaseOutBack(m_aAnimationTimeEmotes[Emote] / ItemAnimationTime);
		const float BaseSize = 50.0f + HoverPhase * 30.0f;
		const float Size = BaseSize * IconPhase;
		
		// 应用透明度
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, IconAlpha);
		
		IGraphics::CQuadItem QuadItem(ScreenCenter.x + Nudge.x, ScreenCenter.y + Nudge.y, Size, Size);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}
	Graphics()->WrapNormal();

	if(GameClient()->m_GameInfo.m_AllowEyeWheel && g_Config.m_ClEyeWheel && GameClient()->m_aLocalIds[g_Config.m_ClDummy] >= 0)
	{
		// 内圈背景 - 延迟于外圈出现
		const float InnerCircleDelay = AnimationTime * 0.15f;
		float InnerAnimTime = IsOpening ? std::max(0.0f, m_AnimationTime - InnerCircleDelay) : m_AnimationTime;
		float InnerNormalizedTime = std::min(1.0f, InnerAnimTime / (AnimationTime - InnerCircleDelay));
		float InnerPhase = AnimationTime == 0.0f ? 1.0f : (IsOpening ? EaseOutBack(InnerNormalizedTime) : EaseInBack(InnerNormalizedTime));
		float InnerAlpha = AnimationTime == 0.0f ? 1.0f : (IsOpening ? std::min(1.0f, InnerNormalizedTime * 1.5f) : InnerNormalizedTime);
		
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.3f * InnerAlpha);
		Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, s_InnerCircleRadius * InnerPhase, 64);
		Graphics()->QuadsEnd();

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[GameClient()->m_aLocalIds[g_Config.m_ClDummy]].m_RenderInfo;

		const float EyeStaggerDelay = AnimationTime * 0.04f;
		
		for(int Emote = 0; Emote < NUM_EMOTES; Emote++)
		{
			float Angle = 2.0f * pi * Emote / NUM_EMOTES;
			if(Angle > pi)
				Angle -= 2.0f * pi;

			// 眼睛表情延迟弹出（延迟于内圈）
			const float EyeDelay = InnerCircleDelay + EyeStaggerDelay * (float)Emote;
			float EyeAnimTime;
			if(IsOpening)
			{
				EyeAnimTime = std::max(0.0f, m_AnimationTime - EyeDelay);
			}
			else
			{
				EyeAnimTime = std::max(0.0f, m_AnimationTime - EyeStaggerDelay * (float)(NUM_EMOTES - 1 - Emote));
			}
			
			const float MaxEyeTime = AnimationTime - EyeDelay;
			const float EyeNormalizedTime = MaxEyeTime > 0.0f ? std::min(1.0f, EyeAnimTime / MaxEyeTime) : 1.0f;
			
			float EyePhase;
			float EyeAlpha;
			if(AnimationTime == 0.0f)
			{
				EyePhase = 1.0f;
				EyeAlpha = 1.0f;
			}
			else if(IsOpening)
			{
				EyePhase = EaseOutBack(EyeNormalizedTime);
				EyeAlpha = std::min(1.0f, EyeNormalizedTime * 2.0f);
			}
			else
			{
				EyePhase = EaseInBack(EyeNormalizedTime);
				EyeAlpha = EyeNormalizedTime;
			}

			const vec2 Nudge = direction(Angle) * s_InnerItemRadius * EyePhase;
			const float HoverPhase = ItemAnimationTime == 0.0f ? (Emote == m_SelectedEyeEmote ? 1.0f : 0.0f) : EaseOutBack(m_aAnimationTimeEyeEmotes[Emote] / ItemAnimationTime);
			TeeInfo.m_Size = (48.0f + HoverPhase * 18.0f) * EyePhase;
			RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, Emote, vec2(-1.0f, 0.0f), ScreenCenter + Nudge, EyeAlpha);
		}

		// 中心圆 - 最后出现
		const float CenterDelay = InnerCircleDelay + EyeStaggerDelay * NUM_EMOTES;
		float CenterAnimTime = IsOpening ? std::max(0.0f, m_AnimationTime - CenterDelay) : m_AnimationTime;
		float CenterMaxTime = AnimationTime - CenterDelay;
		float CenterNormalizedTime = CenterMaxTime > 0.0f ? std::min(1.0f, CenterAnimTime / CenterMaxTime) : 1.0f;
		float CenterPhase = AnimationTime == 0.0f ? 1.0f : (IsOpening ? EaseOutBack(CenterNormalizedTime) : EaseInBack(CenterNormalizedTime));
		float CenterAlpha = AnimationTime == 0.0f ? 1.0f : (IsOpening ? std::min(1.0f, CenterNormalizedTime * 1.5f) : CenterNormalizedTime);
		
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.3f * CenterAlpha);
		Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 30.0f * CenterPhase, 64);
		Graphics()->QuadsEnd();
	}
	else
		m_SelectedEyeEmote = -1;

	// 光标动画
	float CursorAlpha = AnimationTime == 0.0f ? 1.0f : aAlphaPhase[0];
	RenderTools()->RenderCursor(ScreenCenter + m_SelectorMouse, 24.0f, CursorAlpha);
}

void CEmoticon::Emote(int Emoticon)
{
	CNetMsg_Cl_Emoticon Msg;
	Msg.m_Emoticon = Emoticon;
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgDummy(NETMSGTYPE_CL_EMOTICON, false);
		MsgDummy.AddInt(Emoticon);
		Client()->SendMsg(!g_Config.m_ClDummy, &MsgDummy, MSGFLAG_VITAL);
	}
}

void CEmoticon::EyeEmote(int Emote)
{
	char aBuf[32];
	switch(Emote)
	{
	case EMOTE_NORMAL:
		str_format(aBuf, sizeof(aBuf), "/emote normal %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_PAIN:
		str_format(aBuf, sizeof(aBuf), "/emote pain %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_HAPPY:
		str_format(aBuf, sizeof(aBuf), "/emote happy %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_SURPRISE:
		str_format(aBuf, sizeof(aBuf), "/emote surprise %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_ANGRY:
		str_format(aBuf, sizeof(aBuf), "/emote angry %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_BLINK:
		str_format(aBuf, sizeof(aBuf), "/emote blink %d", g_Config.m_ClEyeDuration);
		break;
	}
	GameClient()->m_Chat.SendChat(0, aBuf);
}
