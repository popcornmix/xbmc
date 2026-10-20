/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "DVPlaybackModes.h"
#ifdef HAVE_DVBRIDGE
#include "FileItem.h"
#include "ServiceBroker.h"
#include "GUIDialogNumeric.h"
#include "GUIDialogKaiToast.h"
#include "GUIDialogSelect.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/SettingConditions.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingDefinitions.h"
#include "utils/StringUtils.h"

CDVPlaybackModes& CDVPlaybackModes::GetInstance()
{
  static CDVPlaybackModes instance;
  return instance;
}

bool CDVPlaybackModes::MenuEnabled()
{
  return DVBRIDGE::EnhancedInfoEnabled();
}

bool CDVPlaybackModes::ConfigureProfile()
{
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!settings || !dialog) return false;
  const auto preferences=DVBRIDGE::FrameOutputSettings();
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  std::string peak=StringUtils::Format("{:g}",preferences.peak);
  if (!CGUIDialogNumeric::ShowAndGetNumber(peak,strings.Get(40829))) return false;
  double value{};
  try
  {
    size_t length{};
    value=std::stod(peak,&length);
    if (length!=peak.size() || !std::isfinite(value) || value<=0 || value>10000) return false;
  }
  catch (const std::exception&) { return false; }
  dialog->Reset(); dialog->SetHeading(40836);
  for(int id : {40838,40839,40840}) dialog->Add(strings.Get(id));
  dialog->SetSelected(preferences.gamut>0 ? preferences.gamut-1 : 1);
  dialog->Open();
  if (!dialog->IsConfirmed()) return false;
  const int gamut=dialog->GetSelectedItem()+1;
  auto profile=preferences;
  profile.peak=value; profile.gamut=gamut;
  if (!profile.ProfileValid()) return false;
  {
    std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
    if (!settings->SetNumber("dvbridge.tvpeak",value) ||
        !settings->SetInt("dvbridge.tvgamut",gamut))
    {
      settings->SetNumber("dvbridge.tvpeak",preferences.peak);
      settings->SetInt("dvbridge.tvgamut",preferences.gamut);
      return false;
    }
  }
  settings->Save();
  return true;
}

bool CDVPlaybackModes::ShowMenu()
{
  if (!MenuEnabled()) return false;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog) return false;
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  struct Choice { int output,method,preset,label; };
  constexpr Choice dvChoices[]={{0,0,0,40812},{3,0,0,40861},{3,0,1,40862},
      {2,0,0,40814},{1,0,0,40822},{1,1,0,40823}};
  const auto source=DVBRIDGE::StreamTelemetry::Read().transaction.capabilities;
  if (!source.sourceKnown || (!source.isDV && !source.isHDR10)) return false;
  const bool hdr10Source=source.isHDR10 && !source.isDV;
#ifdef CB1_HAS_HDR10_AI
  constexpr Choice hdr10Choices[]={{1,0,0,40867},{2,0,0,40871},{2,0,1,40872},
      {0,0,0,40866}};
#else
  constexpr Choice hdr10Choices[]={{0,0,0,40866}};
#endif
  const Choice* choices=hdr10Source ? hdr10Choices : dvChoices;
  const int count=hdr10Source ? std::size(hdr10Choices) : std::size(dvChoices);
  const auto choiceSetting=std::make_shared<CSettingInt>(hdr10Source ?
      "dvbridge.hdr10playbackchoice" : "dvbridge.playbackchoice");
  {
    const auto preferences=DVBRIDGE::FrameOutputSettings();
    std::vector<IntegerSettingOption> options;
    int current=-1;
    for(int i=0;i<count;++i)
    {
      options.emplace_back(strings.Get(choices[i].label),i);
      const auto& choice=choices[i];
      if (hdr10Source ? (int(preferences.hdr10Source)==choice.output &&
          (choice.output<2 || preferences.preset==choice.preset)) :
          (int(preferences.output)==choice.output &&
          (choice.output!=1 || preferences.method==choice.method) &&
          (choice.output!=3 || preferences.preset==choice.preset))) current=i;
    }
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    dialog->Reset();
    dialog->SetHeading(StringUtils::Format(strings.Get(40876), hdr10Source ? "HDR10" : "Dolby Vision"));
    dialog->SetUseDetails(true);
    for(const auto& option : options)
    {
      CFileItem item(strings.Get(choices[option.value].label));
      item.SetLabel2(option.label2);
      for(const auto& [name,value] : option.properties) item.SetProperty(name,value);
      dialog->Add(item);
    }
    dialog->SetSelected(current);
    dialog->SetProperty("CB1.PlaybackModes",true);
    dialog->Open();
    dialog->SetProperty("CB1.PlaybackModes", CVariant{});
    if (!dialog->IsConfirmed()) return true;
    const int selected=dialog->GetSelectedItem();
    if (selected<0 || selected>=count) return true;
    // Admission may change during the dialog's nested event loop.
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    for(const auto& [name,value] : options[selected].properties)
      if (name=="disabled" && value.asBoolean()) return true;
    const auto& choice=choices[selected];
    {
      std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
      const int priorOutput=settings->GetInt("dvbridge.outputmode");
      const int priorHDR10=settings->GetInt("dvbridge.hdr10source");
      const int priorMethod=settings->GetInt("dvbridge.hdr10method");
      const int priorPreset=settings->GetInt("dvbridge.enhancement");
      if (!(hdr10Source ? settings->SetInt("dvbridge.hdr10source",choice.output) :
          (settings->SetInt("dvbridge.outputmode",choice.output) &&
           settings->SetInt("dvbridge.hdr10method",choice.method))) ||
          !settings->SetInt("dvbridge.enhancement",choice.preset))
      {
        settings->SetInt("dvbridge.outputmode",priorOutput);
        settings->SetInt("dvbridge.hdr10source",priorHDR10);
        settings->SetInt("dvbridge.hdr10method",priorMethod);
        settings->SetInt("dvbridge.enhancement",priorPreset);
        return true;
      }
#ifdef CB1_HAS_HDR10_AI
      if (hdr10Source && choice.output != 0) ++DVBRIDGE::hdr10Reselection;
#endif
    }
    settings->Save();
    return true;
  }
}

void CDVPlaybackModes::OnSettingAction(const std::shared_ptr<const CSetting>& setting)
{
  if (setting && setting->GetId()=="dvbridge.configureprofile") ConfigureProfile();
}

void DVBRIDGE::PlaybackNotification::Emit(const std::string& text)
{
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  if (settings && settings->GetBool("dvbridge.startupmode"))
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
        CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(40863),text,3000,false);
}
void DVBRIDGE::PlaybackNotification::EmitAI(int mode,int preset)
{
  if (mode!=1 && mode!=4) return;
  const int label=mode==1 ? 40867 : (preset ? 40872 : 40871);
  Emit(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(label));
}
void DVBRIDGE::PlaybackNotification::HDR10Recovery()
{
  const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
      localizer.Get(40863),localizer.Get(40875),3000,false);
}
#endif
