using CredentialManagement;
using Microsoft.Identity.Client;
using System;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Threading.Tasks;
using Windows.Media.Control;
using Windows.UI.Shell;

namespace NanoBridgeApp
{
    public class BridgeSimple
    {
        private readonly string espBaseUrl;
        private readonly string clientId;
        private const string todoListId = ""; // TODO: 여기에 Microsoft To Do 목록 ID를 입력하세요. (예: "AAMkAGI2T...")

        private readonly HttpClient http = new HttpClient();
        private readonly IPublicClientApplication msalApp;

        private readonly string[] scopes = new[]
        {
            "User.Read",
            "Mail.Read",
            "Calendars.Read",
            "Tasks.ReadWrite"
        };

        private IAccount? signedInAccount;

        private static readonly TimeSpan MailInterval = TimeSpan.FromSeconds(15);
        private static readonly TimeSpan TodoInterval = TimeSpan.FromSeconds(45);
        private static readonly TimeSpan CalendarInterval = TimeSpan.FromSeconds(45);
        private static readonly TimeSpan LoopDelay = TimeSpan.FromSeconds(3);

        private DateTimeOffset lastMailSync = DateTimeOffset.MinValue;
        private DateTimeOffset lastTodoSync = DateTimeOffset.MinValue;
        private DateTimeOffset lastCalendarSync = DateTimeOffset.MinValue;

        private string? currentTaskId = null;

        private FocusSessionManager? focusManager;
        private GlobalSystemMediaTransportControlsSessionManager? mediaManager;

        public BridgeSimple()
        {
            SetConsoleStyle();

            espBaseUrl = LoadOrPrompt("Wonjin.NanoBridge.EspBaseUrl", "아두이노 Base URL 입력 (예: http://192.168.0.): ").Trim().TrimEnd('/'); // 아두이노 IP 주소 입력
            clientId = LoadOrPrompt("Wonjin.NanoBridge.ClientId", "Azure Client ID 입력: ").Trim();

            msalApp = PublicClientApplicationBuilder
                .Create(clientId)
                .WithAuthority(AzureCloudInstance.AzurePublic, "consumers")
                .WithDefaultRedirectUri()
                .Build();

            http.Timeout = TimeSpan.FromSeconds(15);
        }

        private void SetConsoleStyle()
        {
            Console.Title = "ESP32 NanoBridge Control Center";
            Console.Clear();
            Console.ForegroundColor = ConsoleColor.Cyan;
            Console.WriteLine("╔══════════════════════════════════════════════════════╗");
            Console.WriteLine("║                                                      ║");
            Console.WriteLine("║           ESP32 NanoBridge Control Center            ║");
            Console.WriteLine("║                                                      ║");
            Console.WriteLine("╚══════════════════════════════════════════════════════╝\n");
            Console.ResetColor();
        }

        private void PrintLog(string message, ConsoleColor color = ConsoleColor.White, bool isImportant = false)
        {
            Console.ForegroundColor = ConsoleColor.DarkGray;
            Console.Write($"[{DateTime.Now:HH:mm:ss}] ");
            Console.ForegroundColor = color;

            if (isImportant)
            {
                Console.WriteLine($"\n>>> {message} <<<\n");
            }
            else
            {
                Console.WriteLine(message);
            }
            Console.ResetColor();
        }

        public async Task RunAsync()
        {
            PrintLog("시스템 초기화를 시작합니다...", ConsoleColor.Cyan);

            StartHttpServer();

            await InitializeLocalMonitorsAsync();

            string token = await GetAccessTokenAsync(forceInteractive: true);
            PrintLog("MS Graph API 인증 완료. 주기적 동기화를 시작합니다.", ConsoleColor.Green);
            Console.WriteLine(new string('-', 56));

            while (true)
            {
                try
                {
                    var now = DateTimeOffset.Now;

                    if (now - lastMailSync >= MailInterval)
                    {
                        token = await GetAccessTokenAsync();
                        await PublishLatestMailAsync(token);
                        lastMailSync = now;
                    }

                    if (now - lastTodoSync >= TodoInterval)
                    {
                        token = await GetAccessTokenAsync();
                        await PublishTodoAsync(token);
                        lastTodoSync = now;
                    }

                    if (now - lastCalendarSync >= CalendarInterval)
                    {
                        token = await GetAccessTokenAsync();
                        await PublishCalendarAsync(token);
                        lastCalendarSync = now;
                    }
                }
                catch (MsalUiRequiredException)
                {
                    PrintLog("토큰 갱신에 사용자 로그인이 필요합니다.", ConsoleColor.Yellow);
                    token = await GetAccessTokenAsync(forceInteractive: true);
                }
                catch (Exception ex)
                {
                    PrintLog($"동기화 루프 오류: {ex.Message}", ConsoleColor.Red);
                }

                await Task.Delay(LoopDelay);
            }
        }

        private async Task InitializeLocalMonitorsAsync()
        {
            if (FocusSessionManager.IsSupported)
            {
                focusManager = FocusSessionManager.GetDefault();
                focusManager.IsFocusActiveChanged += (s, e) => SendFocusState(s.IsFocusActive);
                SendFocusState(focusManager.IsFocusActive);
                PrintLog("윈도우 집중 모드 모니터링 활성화", ConsoleColor.Cyan);
            }

            try
            {
                mediaManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                if (mediaManager != null)
                {
                    mediaManager.CurrentSessionChanged += MediaManager_CurrentSessionChanged;
                    CheckCurrentSession();
                    PrintLog("윈도우 미디어 모니터링 활성화", ConsoleColor.Cyan);
                }
            }
            catch (Exception ex)
            {
                PrintLog($"미디어 권한 오류 (권한이 없을 수 있습니다): {ex.Message}", ConsoleColor.Red);
            }
        }

        private void SendFocusState(bool isActive)
        {
            string state = isActive ? "on" : "off";
            string url = $"{espBaseUrl}/focus?status={state}";
            _ = SendToEspAsync(url);
            PrintLog($"집중 모드 상태 전송: {state.ToUpper()}", ConsoleColor.DarkYellow);
        }

        private void MediaManager_CurrentSessionChanged(GlobalSystemMediaTransportControlsSessionManager sender, CurrentSessionChangedEventArgs args)
        {
            CheckCurrentSession();
        }

        private void CheckCurrentSession()
        {
            if (mediaManager == null) return;
            var session = mediaManager.GetCurrentSession();

            if (session != null)
            {
                session.MediaPropertiesChanged -= Session_MediaPropertiesChanged;
                session.MediaPropertiesChanged += Session_MediaPropertiesChanged;
                session.PlaybackInfoChanged -= Session_PlaybackInfoChanged;
                session.PlaybackInfoChanged += Session_PlaybackInfoChanged;

                _ = UpdateMediaInfo(session);
            }
            else
            {
                _ = SendToEspAsync($"{espBaseUrl}/music?title=Stopped&artist=");
            }
        }

        private void Session_PlaybackInfoChanged(GlobalSystemMediaTransportControlsSession session, PlaybackInfoChangedEventArgs args)
        {
            _ = UpdateMediaInfo(session);
        }

        private void Session_MediaPropertiesChanged(GlobalSystemMediaTransportControlsSession session, MediaPropertiesChangedEventArgs args)
        {
            _ = UpdateMediaInfo(session);
        }

        private async Task UpdateMediaInfo(GlobalSystemMediaTransportControlsSession session)
        {
            try
            {
                var info = session.GetPlaybackInfo();
                if (info.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                {
                    var props = await session.TryGetMediaPropertiesAsync();
                    string title = Uri.EscapeDataString(props.Title ?? "");
                    string artist = Uri.EscapeDataString(props.Artist ?? "");

                    await SendToEspAsync($"{espBaseUrl}/music?title={title}&artist={artist}");
                }
                else if (info.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Paused)
                {
                    await SendToEspAsync($"{espBaseUrl}/music?title=Paused&artist=");
                }
            }
            catch { }
        }

        private void StartHttpServer()
        {
            Task.Run(async () =>
            {
                using var listener = new HttpListener();
                listener.Prefixes.Add("http://+:8080/");
                try
                {
                    listener.Start();
                    PrintLog("ESP 완료 신호 수신 서버 시작 (포트 8080)", ConsoleColor.Green);

                    while (true)
                    {
                        var context = await listener.GetContextAsync();
                        var req = context.Request;
                        var res = context.Response;

                        if (req.Url != null && req.Url.AbsolutePath.Contains("/completeTodo"))
                        {
                            PrintLog("ESP 기기로부터 할 일 완료 요청 수신!", ConsoleColor.Magenta, isImportant: true);
                            await CompleteCurrentTaskAsync();
                        }

                        res.StatusCode = 200;
                        res.Close();
                    }
                }
                catch (Exception ex)
                {
                    PrintLog($"수신 서버 시작 실패 (Visual Studio '관리자 권한' 실행 필요): {ex.Message}", ConsoleColor.Red);
                }
            });
        }

        private async Task CompleteCurrentTaskAsync()
        {
            if (string.IsNullOrEmpty(currentTaskId))
            {
                PrintLog("완료할 Task ID가 현재 없습니다.", ConsoleColor.Yellow);
                return;
            }

            try
            {
                string token = await GetAccessTokenAsync();
                string url = $"https://graph.microsoft.com/v1.0/me/todo/lists/{todoListId}/tasks/{currentTaskId}";

                using var req = new HttpRequestMessage(HttpMethod.Patch, url);
                req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
                req.Content = new StringContent("{\"status\":\"completed\"}", System.Text.Encoding.UTF8, "application/json");

                using var res = await http.SendAsync(req);
                if (res.IsSuccessStatusCode)
                {
                    PrintLog("MS Graph: 할 일 완료 처리 성공!", ConsoleColor.Green);
                    currentTaskId = null;
                    await PublishTodoAsync(token);
                    lastTodoSync = DateTimeOffset.Now;
                }
                else
                {
                    PrintLog($"할 일 완료 실패 (HTTP {res.StatusCode})", ConsoleColor.Red);
                }
            }
            catch (Exception ex)
            {
                PrintLog($"완료 처리 중 에러: {ex.Message}", ConsoleColor.Red);
            }
        }

        private async Task<string> GetAccessTokenAsync(bool forceInteractive = false)
        {
            if (!forceInteractive)
            {
                try
                {
                    if (signedInAccount == null)
                        signedInAccount = (await msalApp.GetAccountsAsync()).FirstOrDefault();

                    if (signedInAccount != null)
                    {
                        var silentResult = await msalApp.AcquireTokenSilent(scopes, signedInAccount).ExecuteAsync();
                        return silentResult.AccessToken;
                    }
                }
                catch (MsalUiRequiredException) { }
            }

            var accounts = (await msalApp.GetAccountsAsync()).ToList();
            signedInAccount = accounts.FirstOrDefault();

            var interactiveBuilder = msalApp.AcquireTokenInteractive(scopes).WithPrompt(Prompt.SelectAccount);
            if (signedInAccount != null) interactiveBuilder = interactiveBuilder.WithAccount(signedInAccount);

            var result = await interactiveBuilder.ExecuteAsync();
            signedInAccount = result.Account;

            return result.AccessToken;
        }

        private async Task PublishLatestMailAsync(string accessToken)
        {
            string url = "https://graph.microsoft.com/v1.0/me/messages?$top=1&$select=subject,from,receivedDateTime&$orderby=receivedDateTime%20desc";
            using var req = new HttpRequestMessage(HttpMethod.Get, url);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
            using var res = await http.SendAsync(req);

            if (!res.IsSuccessStatusCode) return;

            string json = await res.Content.ReadAsStringAsync();
            using var doc = JsonDocument.Parse(json);
            var items = doc.RootElement.GetProperty("value");

            if (items.GetArrayLength() == 0) return;

            var mail = items[0];
            string subject = mail.GetProperty("subject").GetString() ?? "(제목 없음)";
            string from = mail.GetProperty("from").GetProperty("emailAddress").GetProperty("address").GetString() ?? "";

            PrintLog($"최신 메일 감지: [{from}] {subject}", ConsoleColor.DarkGray);

            await SendToEspAsync($"{espBaseUrl}/mail?subject={Uri.EscapeDataString(subject)}&from={Uri.EscapeDataString(from)}");
        }

        private async Task PublishTodoAsync(string accessToken)
        {
            string url = $"https://graph.microsoft.com/v1.0/me/todo/lists/{todoListId}/tasks";
            using var req = new HttpRequestMessage(HttpMethod.Get, url);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
            using var res = await http.SendAsync(req);

            if (!res.IsSuccessStatusCode) return;

            string json = await res.Content.ReadAsStringAsync();
            using var doc = JsonDocument.Parse(json);
            var items = doc.RootElement.GetProperty("value");

            string? firstActiveTitle = null;
            string? firstActiveId = null;

            for (int i = 0; i < items.GetArrayLength(); i++)
            {
                var task = items[i];
                string title = task.GetProperty("title").GetString() ?? "(제목 없음)";
                string status = task.TryGetProperty("status", out var st) ? st.GetString() ?? "" : "";

                if (string.Equals(status, "completed", StringComparison.OrdinalIgnoreCase)) continue;

                if (firstActiveTitle == null)
                {
                    firstActiveTitle = title;
                    firstActiveId = task.GetProperty("id").GetString();
                }
            }

            currentTaskId = firstActiveId;

            if (string.IsNullOrWhiteSpace(firstActiveTitle))
            {
                await SendToEspAsync($"{espBaseUrl}/todo?title={Uri.EscapeDataString("(할 일 없음)")}");
                return;
            }

            PrintLog($"할 일 감지됨: {firstActiveTitle}", ConsoleColor.DarkGray);
            await SendToEspAsync($"{espBaseUrl}/todo?title={Uri.EscapeDataString(firstActiveTitle)}");
        }

        private async Task PublishCalendarAsync(string accessToken)
        {
            var now = DateTimeOffset.Now;
            var start = new DateTimeOffset(now.Year, now.Month, now.Day, 0, 0, 0, now.Offset);
            var end = start.AddDays(1);

            string url = $"https://graph.microsoft.com/v1.0/me/calendarView?startDateTime={Uri.EscapeDataString(start.ToString("o"))}&endDateTime={Uri.EscapeDataString(end.ToString("o"))}&$top=1&$orderby=start/dateTime&$select=subject,start";

            using var req = new HttpRequestMessage(HttpMethod.Get, url);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
            using var res = await http.SendAsync(req);

            if (!res.IsSuccessStatusCode) return;

            string json = await res.Content.ReadAsStringAsync();
            using var doc = JsonDocument.Parse(json);
            var items = doc.RootElement.GetProperty("value");

            string subject = "(일정 없음)";
            string time = "";

            if (items.GetArrayLength() > 0)
            {
                var ev = items[0];
                subject = ev.GetProperty("subject").GetString() ?? "(일정 없음)";
                time = ev.GetProperty("start").GetProperty("dateTime").GetString() ?? "";

                PrintLog($"오늘의 일정 감지: {subject}", ConsoleColor.DarkGray);
            }

            await SendToEspAsync($"{espBaseUrl}/calendar?time={Uri.EscapeDataString(time)}&subject={Uri.EscapeDataString(subject)}");
        }

        private async Task SendToEspAsync(string url)
        {
            try { await http.GetAsync(url); } catch { }
        }

        private static string LoadOrPrompt(string target, string prompt)
        {
            using var cred = new Credential { Target = target, Type = CredentialType.Generic };
            if (cred.Load() && !string.IsNullOrWhiteSpace(cred.Password)) return cred.Password;

            Console.ForegroundColor = ConsoleColor.White;
            Console.Write(prompt);
            Console.ForegroundColor = ConsoleColor.Cyan;
            string input = Console.ReadLine() ?? "";
            Console.ResetColor();

            using var saveCred = new Credential { Target = target, Type = CredentialType.Generic, Username = "app", Password = input.Trim(), PersistanceType = PersistanceType.LocalComputer };
            saveCred.Save();
            return input.Trim();
        }
    }
}