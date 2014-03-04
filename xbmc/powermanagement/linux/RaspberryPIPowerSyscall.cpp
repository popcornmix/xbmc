/*
 *      Copyright (C) 2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if defined(TARGET_RASPBERRY_PI)

#include "system.h"
#include "RaspberryPIPowerSyscall.h"
#if defined(HAS_DBUS)
#include "LogindUPowerSyscall.h"
#endif
#include "RBP.h"

bool CRaspberryPIPowerSyscall::VirtualSleep()
{
  g_RBP.SuspendVideoOutput();
  return true;
}

bool CRaspberryPIPowerSyscall::VirtualWake()
{
  g_RBP.ResumeVideoOutput();
  return true;
}

bool CRaspberryPIPowerSyscall::Powerdown()
{
  int s = false;
#if defined(HAS_DBUS)
  if (CLogindUPowerSyscall::HasLogind())
  {
    IPowerSyscall *m_instance = new CLogindUPowerSyscall;
    if (m_instance->CanPowerdown())
      s = m_instance->Powerdown();
    delete m_instance;
  }
#endif
  return s;
}

bool CRaspberryPIPowerSyscall::Reboot()
{
  int s = false;
#if defined(HAS_DBUS)
  if (CLogindUPowerSyscall::HasLogind())
  {
    IPowerSyscall *m_instance = new CLogindUPowerSyscall;
    if (m_instance->CanReboot())
      s = m_instance->Reboot();
    delete m_instance;
  }
#endif
  return s;
}

#endif
