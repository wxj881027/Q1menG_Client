#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_SIDEBAR_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_SIDEBAR_H

#include <game/client/component.h>
#include <functional>

enum
{
	SIDEBAR_MAX_ITEMS = 32,
	SIDEBAR_ITEM_TYPE_LETTERS = 4
};

class CSidebarItem
{
public:
	std::function<void(float X, float Y, float Width, float Height)> m_RenderItem;
	std::function<float()> m_GetHeight;
	char m_aName[32];
	char m_aDisplayName[32];
	char m_aDesc[128];
	char m_aLetters[SIDEBAR_ITEM_TYPE_LETTERS] = {};
	bool m_ShowLabel = true;
	
	CSidebarItem(std::function<void(float, float, float, float)> Render, 
		std::function<float()> Height, 
		const char *pLetters, 
		const char *pName, 
		const char *pDisplayName, 
		const char *pDesc, 
		bool ShowLabel = true);
};

class CSidebar : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;
	void OnInit() override;

	// Sidebar items
	CSidebarItem m_FPS = CSidebarItem([this](float X, float Y, float W, float H) { FPSRender(X, Y, W, H); }, 
		std::bind(&CSidebar::FPSHeight, this),
		"f", "FPS", "", "Displays frames per second");
		
	CSidebarItem m_Ping = CSidebarItem([this](float X, float Y, float W, float H) { PingRender(X, Y, W, H); }, 
		std::bind(&CSidebar::PingHeight, this),
		"p", "Ping", "", "Displays ping to server");
		
	CSidebarItem m_Position = CSidebarItem([this](float X, float Y, float W, float H) { PositionRender(X, Y, W, H); }, 
		std::bind(&CSidebar::PositionHeight, this),
		"c", "Position", "Pos", "Displays position coordinates");
		
	CSidebarItem m_Velocity = CSidebarItem([this](float X, float Y, float W, float H) { VelocityRender(X, Y, W, H); }, 
		std::bind(&CSidebar::VelocityHeight, this),
		"v", "Velocity", "Vel", "Displays velocity");
		
	CSidebarItem m_Angle = CSidebarItem([this](float X, float Y, float W, float H) { AngleRender(X, Y, W, H); }, 
		std::bind(&CSidebar::AngleHeight, this),
		"a", "Angle", "", "Displays aiming angle");
		
	CSidebarItem m_LocalTime = CSidebarItem([this](float X, float Y, float W, float H) { LocalTimeRender(X, Y, W, H); }, 
		std::bind(&CSidebar::LocalTimeHeight, this),
		"t", "Time", "", "Displays local time");
		
	CSidebarItem m_Space = CSidebarItem([this](float X, float Y, float W, float H) { SpaceRender(X, Y, W, H); }, 
		std::bind(&CSidebar::SpaceHeight, this),
		" _", "Space", " ", "Gap between items", false);

	std::vector<CSidebarItem> m_SidebarItemTypes = {
		m_FPS, m_Ping, m_Position, m_Velocity, m_Angle, m_LocalTime, m_Space
	};
	
	std::vector<CSidebarItem *> m_SidebarItems = {
		&m_FPS, &m_Space, &m_Ping, &m_Space, &m_Position
	};

	void ApplySidebarScheme(const char *pScheme);
	void UpdateSidebarScheme(char *pScheme);

private:
	float m_FrameTimeAverage = 0.0f;
	int m_PlayerId = 0;
	float m_FontSize = 12.0f;
	float m_SidebarX, m_SidebarY;
	float m_SidebarWidth;
	float m_ItemHeight, m_Margin;
	
	// Render functions
	float FPSHeight();
	void FPSRender(float X, float Y, float Width, float Height);
	
	float PingHeight();
	void PingRender(float X, float Y, float Width, float Height);
	
	float PositionHeight();
	void PositionRender(float X, float Y, float Width, float Height);
	
	float VelocityHeight();
	void VelocityRender(float X, float Y, float Width, float Height);
	
	float AngleHeight();
	void AngleRender(float X, float Y, float Width, float Height);
	
	float LocalTimeHeight();
	void LocalTimeRender(float X, float Y, float Width, float Height);
	
	float SpaceHeight();
	void SpaceRender(float X, float Y, float Width, float Height);
	
	void LabelRender(float X, float Y, const char *pLabel);
	float LabelHeight();
};

#endif
