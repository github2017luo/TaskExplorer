#pragma once
#include "../SocketInfo.h"

class CWinSocket : public CSocketInfo
{
	Q_OBJECT
public:
	CWinSocket(QObject *parent = nullptr);
	virtual ~CWinSocket();

	static QHostAddress PH2QAddress(struct _PH_IP_ADDRESS* addr);

	virtual QString			GetOwnerServiceName()	{ QReadLocker Locker(&m_Mutex); return m_OwnerService; }
	virtual bool			IsSubsystemProcess()	{ QReadLocker Locker(&m_Mutex); return m_SubsystemProcess; }

	virtual QString			GetFirewallStatus();

	bool Match(struct _PH_NETWORK_CONNECTION* connection);

protected:
	friend class CWindowsAPI;

	bool InitStaticData(struct _PH_NETWORK_CONNECTION* connection);

	bool UpdateDynamicData(struct _PH_NETWORK_CONNECTION* connection);

	QString			m_OwnerService;
	bool			m_SubsystemProcess;


private:
	struct SWinSocket* m;
};