#include "src/run/cli.h"
#include <limits>
#include <utility>

namespace myrd { namespace {
constexpr std::uint64_t kKiB=1024ull,kMiB=1024ull*kKiB,kGiB=1024ull*kMiB,kTiB=1024ull*kGiB;
CliParseResult error(std::wstring m){return {CliAction::Error,{}, {},std::move(m)};}
bool decimal(std::wstring_view s,std::uint64_t& out) noexcept {if(s.empty())return false;out=0; constexpr auto max=std::numeric_limits<std::uint64_t>::max(); for(wchar_t c:s){if(c<L'0'||c>L'9')return false;auto d=static_cast<std::uint64_t>(c-L'0');if(out>(max-d)/10)return false;out=out*10+d;}return true;}
bool binary_size(std::wstring_view s,std::uint64_t& out) noexcept {if(s.size()<2)return false;std::uint64_t scale=0;switch(s.back()){case L'K':scale=kKiB;break;case L'M':scale=kMiB;break;case L'G':scale=kGiB;break;case L'T':scale=kTiB;break;default:return false;}std::uint64_t n{};if(!decimal(s.substr(0,s.size()-1),n)||n==0||n>std::numeric_limits<std::uint64_t>::max()/scale)return false;out=n*scale;return true;}
bool prefix(std::wstring_view v,std::wstring_view p) noexcept {if(v.size()<p.size())return false;for(size_t i=0;i<p.size();++i){wchar_t a=v[i],b=p[i];if(a>=L'A'&&a<=L'Z')a=static_cast<wchar_t>(a-L'A'+L'a');if(b>=L'A'&&b<=L'Z')b=static_cast<wchar_t>(b-L'A'+L'a');if(a!=b)return false;}return true;}
bool valid_bundle(std::wstring_view p) noexcept {if(p.empty()||p.find(L'\0')!=std::wstring_view::npos)return false;for(auto x:{L"\\\\.\\",L"\\\\?\\",L"\\??\\",L"\\Device\\",L"\\GLOBAL??\\",L"\\DosDevices\\"})if(prefix(p,x))return false;auto c=p.find(L':');if(c==std::wstring_view::npos)return true;if(c!=1||!((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z')))return false;return p.find(L':',c+1)==std::wstring_view::npos;}
bool drive(std::wstring_view v,wchar_t& out) noexcept {if(v.size()!=2||v[1]!=L':')return false; if(!((v[0]>=L'A'&&v[0]<=L'Z')||(v[0]>=L'a'&&v[0]<=L'z')))return false;out=v[0]>=L'a'&&v[0]<=L'z'?static_cast<wchar_t>(v[0]-L'a'+L'A'):v[0];return true;}
}}
namespace myrd {
CliParseResult parse_cli(std::span<const std::wstring_view> a){
 if(a.size()==1&&a[0]==L"--help")return {CliAction::Help}; if(a.empty()||(a[0]!=L"run"&&a[0]!=L"compact"))return error(L"unknown command"); bool r=a[0]==L"run"; if(a.size()==2&&a[1]==L"--help")return {CliAction::Help};
 std::optional<std::wstring_view>b,s,d,t,dm,ram; for(size_t i=1;i<a.size();){auto k=a[i++];if(i>=a.size()||a[i].empty()||a[i].starts_with(L"--"))return error(L"missing option value");auto v=a[i++];auto set=[&](auto& x){if(x)return false;x=v;return true;}; if(k==L"--bundle"){if(!set(b))return error(L"duplicate option");}else if(r&&k==L"--size"){if(!set(s))return error(L"duplicate option");}else if(r&&k==L"--drive-letter"){if(!set(d))return error(L"duplicate option");}else if(r&&k==L"--checkpoint-seconds"){if(!set(t))return error(L"duplicate option");}else if(r&&k==L"--checkpoint-dirty-mb"){if(!set(dm))return error(L"duplicate option");}else if(r&&k==L"--ram-limit"){if(!set(ram))return error(L"duplicate option");}else return error(L"unknown option");}
 if(!b||!valid_bundle(*b))return error(L"invalid bundle path"); if(!r){if(s||d||t||dm||ram)return error(L"option is not valid for compact");return {CliAction::Compact,{},CompactOptions{std::filesystem::path(*b)}, {}};} if(!s||!d)return error(L"missing required option"); RunOptions o; o.bundle=std::filesystem::path(*b);if(!drive(*d,o.drive_letter))return error(L"invalid drive letter");if(!binary_size(*s,o.virtual_bytes)||o.virtual_bytes<64*kMiB||o.virtual_bytes>kTiB)return error(L"invalid size");std::uint64_t n{};if(t&&(!decimal(*t,n)||n<5||n>86400))return error(L"invalid checkpoint seconds");if(t)o.checkpoint_interval=std::chrono::seconds(n);if(dm&&(!decimal(*dm,n)||n<1||n>1048576))return error(L"invalid checkpoint dirty mb");if(dm)o.checkpoint_dirty_bytes=n*kMiB;if(ram&&(!binary_size(*ram,n)||n<64*kMiB||n>kTiB))return error(L"invalid ram limit");if(ram)o.ram_limit_bytes=n;return {CliAction::Run,std::move(o),{}, {}};
}
}
