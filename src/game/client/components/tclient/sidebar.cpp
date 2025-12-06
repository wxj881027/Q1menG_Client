#include "sidebar.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>

CSidebarItem::CSidebarItem(std::function<void(float, float, float, float)> Render, 
	std::function<float()> Height, 
	const char *pLetters, 
	const char *pName, 
	const char *pDisplayName, 
	const char *pDesc, 
	bool ShowLabel)
{
	m_RenderItem = std::move(Render);
	m_GetHeight = std::move(Height);
	str_copy(m_aLetters, pLetters);
	str_copy(m_aName, pName);
	if(str_comp(pDisplayName, "") != 0)
		str_copy(m_aDisplayName, pDisplayName);
	else
		str_copy(m_aDisplayName, pName);
	str_copy(m_aDesc, pDesc);
	m_ShowLabel = ShowLabel;
}

void CSidebar::OnInit()
{
	m_SidebarWidth = 120.0f;
	m_ItemHeight = 20.0f;
	m_Margin = 5.0f;
	m_FontSize = 12.0f;
	
	ApplySidebarScheme(g_Config.m_TcSidebarScheme);
}

void CSidebar::ApplySidebarScheme(const char *pScheme)
{
	m_SidebarItems.clear();
	for(int i = 0; pScheme[i] != '\0'; ++i)
	{
		char SchemeLetter = pScheme[i];
		for(CSidebarItem &ItemType : m_SidebarItemTypes)
		{
			for(char ItemLetter : ItemType.m_aLetters)
			{
				if(ItemLetter == SchemeLetter)
				{
					m_SidebarItems.push_back(&ItemType);
					break;
				}
			}
		}
	}
}

void CSidebar::UpdateSidebarScheme(char *pScheme)
{
	int Index = 0;
	for(CSidebarItem *&pItem : m_SidebarItems)
	{
		if(Index >= SIDEBAR_MAX_ITEMS)
			break;
		pScheme[Index++] = pItem->m_aLetters[0];
	}
	pScheme[Index] = '\0';
}

void CSidebar::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(!g_Config.m_TcSidebar || !GameClient()->m_Snap.m_pGameInfoObj)
		return;

	m_PlayerId = GameClient()->m_Snap.m_LocalClientId;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		m_PlayerId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

	const float ScreenWidth = 300.0f * Graphics()->ScreenAspect();
	const float ScreenHeight = 300.0f;
	
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);

	// Position sidebar on right side
	m_SidebarX = ScreenWidth - m_SidebarWidth - m_Margin;
	m_SidebarY = m_Margin * 2.0f;

	// Calculate total height
	float TotalHeight = m_Margin;
	for(const CSidebarItem *pItem : m_SidebarItems)
	{
		float ItemHeight = pItem->m_GetHeight();
		TotalHeight += ItemHeight + m_Margin;
	}

	// Draw background
	Graphics()->DrawRect(m_SidebarX, m_SidebarY, m_SidebarWidth, TotalHeight, 
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcSidebarColor)).WithAlpha(g_Config.m_TcSidebarAlpha / 100.0f), 
		IGraphics::CORNER_ALL, 5.0f);

	// Render items
	float CurrentY = m_SidebarY + m_Margin;
	TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcSidebarTextColor)).WithAlpha(g_Config.m_TcSidebarTextAlpha / 100.0f));
	
	for(const CSidebarItem *pItem : m_SidebarItems)
	{
		float ItemHeight = pItem->m_GetHeight();
		if(ItemHeight > 0.0f)
		{
			pItem->m_RenderItem(m_SidebarX + m_Margin, CurrentY, m_SidebarWidth - m_Margin * 2, ItemHeight);
			CurrentY += ItemHeight + m_Margin;
		}
	}
	
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
}

// FPS
float CSidebar::FPSHeight() { return m_ItemHeight; }
void CSidebar::FPSRender(float X, float Y, float Width, float Height)
{
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + Client()->RenderFrameTime() * 0.1f;
	int FPS = (int)(1.0f / m_FrameTimeAverage + 0.5f);
	
	char aBuf[64];
	if(g_Config.m_TcSidebarLabels)
	{
		str_format(aBuf, sizeof(aBuf), "FPS: %d", FPS);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%d", FPS);
	}
	
	TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
}

// Ping
float CSidebar::PingHeight() 
{
	if(!GameClient()->m_Snap.m_apPlayerInfos[m_PlayerId])
		return 0.0f;
	return m_ItemHeight; 
}

void CSidebar::PingRender(float X, float Y, float Width, float Height)
{
	const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[m_PlayerId];
	
	char aBuf[64];
	if(g_Config.m_TcSidebarLabels)
	{
		str_format(aBuf, sizeof(aBuf), "Ping: %d", pInfo->m_Latency);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%d", pInfo->m_Latency);
	}
	
	TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
}

// Position
float CSidebar::PositionHeight()
{
	if(!GameClient()->m_Snap.m_apPlayerInfos[m_PlayerId])
		return 0.0f;
	return m_ItemHeight;
}

void CSidebar::PositionRender(float X, float Y, float Width, float Height)
{
	const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_Prev;
	const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_Cur;
	const float IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
	const vec2 Pos = mix(vec2(pPrevChar->m_X, pPrevChar->m_Y), vec2(pCurChar->m_X, pCurChar->m_Y), IntraTick) / 32.0f;
	
	char aBuf[128];
	if(g_Config.m_TcSidebarLabels)
	{
		str_format(aBuf, sizeof(aBuf), "Pos: %.1f, %.1f", Pos.x, Pos.y);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%.1f, %.1f", Pos.x, Pos.y);
	}
	
	TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
}

// Velocity
float CSidebar::VelocityHeight()
{
	if(!GameClient()->m_Snap.m_apPlayerInfos[m_PlayerId])
		return 0.0f;
	return m_ItemHeight;
}

void CSidebar::VelocityRender(float X, float Y, float Width, float Height)
{
	const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_Prev;
	const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_Cur;
	const float IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
	const vec2 Vel = mix(vec2(pPrevChar->m_VelX, pPrevChar->m_VelY), vec2(pCurChar->m_VelX, pCurChar->m_VelY), IntraTick);
	
	float VelspeedX = Vel.x / 256.0f * Client()->GameTickSpeed();
	if(Vel.x >= -1 && Vel.x <= 1)
		VelspeedX = 0;
	float VelspeedY = Vel.y / 256.0f * Client()->GameTickSpeed();
	if(Vel.y >= -128 && Vel.y <= 128)
		VelspeedY = 0;
		
	float DisplaySpeedX = VelspeedX / 32;
	float VelspeedLength = length(vec2(Vel.x, Vel.y) / 256.0f) * Client()->GameTickSpeed();
	float Ramp = VelocityRamp(VelspeedLength, 
		GameClient()->m_aTuning[g_Config.m_ClDummy].m_VelrampStart, 
		GameClient()->m_aTuning[g_Config.m_ClDummy].m_VelrampRange, 
		GameClient()->m_aTuning[g_Config.m_ClDummy].m_VelrampCurvature);
	DisplaySpeedX *= Ramp;
	float DisplaySpeedY = VelspeedY / 32;

	char aBuf[128];
	if(g_Config.m_TcSidebarLabels)
	{
		str_format(aBuf, sizeof(aBuf), "Vel: %.1f, %.1f", DisplaySpeedX, DisplaySpeedY);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%.1f, %.1f", DisplaySpeedX, DisplaySpeedY);
	}
	
	TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
}

// Angle
float CSidebar::AngleHeight()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW)
		return 0.0f;
	return m_ItemHeight;
}

void CSidebar::AngleRender(float X, float Y, float Width, float Height)
{
	CNetObj_Character *pCharacter = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_Cur;
	float Angle = 0.0f;
	if(GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_HasExtendedDisplayInfo)
	{
		CNetObj_DDNetCharacter *pExtendedData = &GameClient()->m_Snap.m_aCharacters[m_PlayerId].m_ExtendedData;
		Angle = atan2f(pExtendedData->m_TargetY, pExtendedData->m_TargetX);
	}
	else
		Angle = pCharacter->m_Angle / 256.0f;
		
	if(Angle < 0)
		Angle += 2.0f * pi;
		
	char aBuf[64];
	if(g_Config.m_TcSidebarLabels)
	{
		str_format(aBuf, sizeof(aBuf), "Angle: %.1f°", Angle * 180.0f / pi);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%.1f°", Angle * 180.0f / pi);
	}
	
	TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
}

// Local Time
float CSidebar::LocalTimeHeight() { return m_ItemHeight; }
void CSidebar::LocalTimeRender(float X, float Y, float Width, float Height)
{
	static char s_aTimeBuf[32];
	str_timestamp_format(s_aTimeBuf, sizeof(s_aTimeBuf), "%H:%M:%S");
	
	if(g_Config.m_TcSidebarLabels)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Time: %s", s_aTimeBuf);
		TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, aBuf);
	}
	else
	{
		TextRender()->Text(X, Y + (Height - m_FontSize) / 2, m_FontSize, s_aTimeBuf);
	}
}

// Space
float CSidebar::SpaceHeight() { return m_Margin; }
void CSidebar::SpaceRender(float X, float Y, float Width, float Height) {}

void CSidebar::LabelRender(float X, float Y, const char *pLabel)
{
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "%s:", pLabel);
	TextRender()->Text(X, Y, m_FontSize, aBuf);
}

float CSidebar::LabelHeight()
{
	return m_FontSize;
}
