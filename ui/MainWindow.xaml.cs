using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Data;
using System.Windows.Threading;
using Microsoft.Win32;
using RA2Hook.RuntimeUI.Models;
using RA2Hook.RuntimeUI.Services;

namespace RA2Hook.RuntimeUI;

public partial class MainWindow : Window
{
    private readonly PipeClient _pipe = new();
    private readonly ObservableCollection<RuntimePatchFile> _files = new();
    private readonly ObservableCollection<RuntimeItem> _items = new();
    private readonly ObservableCollection<DumpUnitInfo> _dumpUnits = new();
    private readonly DumpUnitExtractor _dumpExtractor = new();
    private readonly DispatcherTimer _pollTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private FileSystemWatcher? _watcher;
    private string _gameRoot = string.Empty;
    private string _runtimeDirectory = string.Empty;
    private string? _selectedPath;
    private bool _loadingEditor;
    private bool _editorDirty;
    private bool _suppressSelection;
    private bool _syncingAuto;
    private int _lastGeneration = -1;
    private string _lastMessage = string.Empty;
    private bool _polling;
    private bool _loadingDump;
    private string? _lastUnitOutput;

    public MainWindow()
    {
        InitializeComponent();
        FilesList.ItemsSource = _files;
        RuntimeGrid.ItemsSource = _items;
        DumpUnitsList.ItemsSource = _dumpUnits;
        _pollTimer.Tick += async (_, _) => await PollAsync();
        Loaded += async (_, _) =>
        {
            GameRootText.Text = DiscoverGameRoot();
            ConfigureRoot();
            await RefreshDumpCatalogAsync();
            await PollAsync();
            _pollTimer.Start();
        };
        Closed += (_, _) =>
        {
            _pollTimer.Stop();
            _watcher?.Dispose();
        };
        Closing += (_, args) =>
        {
            if (!_editorDirty) return;
            if (MessageBox.Show("当前补丁尚未保存，确定关闭？", "RA2Hook Runtime",
                    MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
                args.Cancel = true;
        };
    }

    private string DiscoverGameRoot()
    {
        // The published executable lives in <game>\ra2hook alongside dump,
        // inject, and runtime. Its parent is therefore always the game root.
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }

    private void ConfigureRoot()
    {
        _gameRoot = Path.GetFullPath(GameRootText.Text.Trim());
        _pipe.SetGameRoot(_gameRoot);
        _runtimeDirectory = IniDocument.ResolveRuntimeDirectory(_gameRoot);
        RuntimePathText.Text = _runtimeDirectory;
        _watcher?.Dispose();
        Directory.CreateDirectory(_runtimeDirectory);
        _watcher = new FileSystemWatcher(_runtimeDirectory)
        {
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite | NotifyFilters.Size,
            IncludeSubdirectories = false,
            EnableRaisingEvents = true
        };
        _watcher.Created += FilesChanged;
        _watcher.Changed += FilesChanged;
        _watcher.Deleted += FilesChanged;
        _watcher.Renamed += FilesChanged;
        RefreshFiles();
    }

    private void FilesChanged(object sender, FileSystemEventArgs args)
    {
        var relevant = RuntimePatchFile.IsPatchPath(args.FullPath) ||
                       args is RenamedEventArgs renamed &&
                       RuntimePatchFile.IsPatchPath(renamed.OldFullPath);
        if (!relevant) return;
        Dispatcher.BeginInvoke(new Action(() =>
        {
            if (_editorDirty)
            {
                StatusText.Text = "目录内容已变化；请先保存当前补丁，再刷新文件";
                return;
            }
            RefreshFiles();
        }), DispatcherPriority.Background);
    }

    private void RefreshFiles(object? sender = null, RoutedEventArgs? args = null)
    {
        if (!Directory.Exists(_runtimeDirectory)) return;
        var selected = _selectedPath;
        _files.Clear();
        foreach (var file in Directory.EnumerateFiles(_runtimeDirectory)
                     .Where(RuntimePatchFile.IsPatchPath)
                     .Select(RuntimePatchFile.FromPath)
                     .OrderBy(value => value.Name, StringComparer.OrdinalIgnoreCase)
                     .ThenByDescending(value => value.IsEnabled))
            _files.Add(file);
        var next = _files.FirstOrDefault(value => value.FullName.Equals(selected,
            StringComparison.OrdinalIgnoreCase)) ?? _files.FirstOrDefault();
        FilesList.SelectedItem = next;
        if (next is null) SetSelectedPatch(null);
    }

    private async void PatchSelected(object sender, System.Windows.Controls.SelectionChangedEventArgs args)
    {
        if (_suppressSelection) return;
        if (FilesList.SelectedItem is not RuntimePatchFile file) return;
        if (_editorDirty && _selectedPath is not null &&
            !file.FullName.Equals(_selectedPath, StringComparison.OrdinalIgnoreCase))
        {
            if (MessageBox.Show("当前补丁尚未保存，确定切换文件？", "RA2Hook Runtime",
                    MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
            {
                _suppressSelection = true;
                FilesList.SelectedItem = _files.FirstOrDefault(value =>
                    value.FullName.Equals(_selectedPath, StringComparison.OrdinalIgnoreCase));
                _suppressSelection = false;
                return;
            }
        }
        if (_editorDirty && file.FullName.Equals(_selectedPath,
                StringComparison.OrdinalIgnoreCase)) return;

        _selectedPath = file.FullName;
        SetSelectedPatch(file);
        _loadingEditor = true;
        try
        {
            EditorText.Text = await File.ReadAllTextAsync(file.FullName);
            SelectedFileText.Text = file.FullName;
            LogText.Text = string.Join(Environment.NewLine,
                IniDocument.Parse(EditorText.Text).Select(row =>
                    $"[{row.Section}] {row.Key} = {row.Value}  ({row.Safety})"));
            _editorDirty = false;
        }
        catch (Exception exception)
        {
            StatusText.Text = $"读取失败: {exception.Message}";
        }
        finally
        {
            _loadingEditor = false;
        }
    }

    private void SetSelectedPatch(RuntimePatchFile? file)
    {
        PatchNameText.Text = file?.Name ?? string.Empty;
        PatchNameText.IsEnabled = file is not null;
        RenameButton.IsEnabled = file is not null;
        if (file is null) SelectedFileText.Text = "未选择文件";
    }

    private void EditorChanged(object sender, System.Windows.Controls.TextChangedEventArgs args)
    {
        if (_loadingEditor) return;
        _editorDirty = _selectedPath is not null;
        var rows = IniDocument.Parse(EditorText.Text);
        StatusText.Text = _selectedPath is null ? "未选择文件" : $"未保存，{rows.Count} 个键";
    }

    private async void SavePatch(object sender, RoutedEventArgs args)
    {
        if (_selectedPath is null) return;
        try
        {
            await IniDocument.WriteAtomicAsync(_selectedPath, EditorText.Text);
            _editorDirty = false;
            StatusText.Text = "已原子保存";
            RefreshFiles();
        }
        catch (Exception exception)
        {
            StatusText.Text = $"保存失败: {exception.Message}";
        }
    }

    private async void NewPatch(object sender, RoutedEventArgs args)
    {
        if (_editorDirty && MessageBox.Show("当前补丁尚未保存，确定新建文件？",
                "RA2Hook Runtime", MessageBoxButton.OKCancel,
                MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        try
        {
            var timestamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var name = $"patch-{timestamp}";
            var suffix = 1;
            while (PatchNameExists(name))
                name = $"patch-{timestamp}-{suffix++}";
            var path = RuntimePatchFile.BuildPath(_runtimeDirectory, name, true);
            await IniDocument.WriteAtomicAsync(path,
                "; RA2Hook runtime patch\r\n[General]\r\n");
            _editorDirty = false;
            _selectedPath = path;
            RefreshFiles();
        }
        catch (Exception exception)
        {
            StatusText.Text = $"新建失败: {exception.Message}";
        }
    }

    private bool PatchNameExists(string name) =>
        File.Exists(RuntimePatchFile.BuildPath(_runtimeDirectory, name, true)) ||
        File.Exists(RuntimePatchFile.BuildPath(_runtimeDirectory, name, false));

    private void RenamePatch(object sender, RoutedEventArgs args)
    {
        if (FilesList.SelectedItem is not RuntimePatchFile file) return;
        var name = NormalizePatchName(PatchNameText.Text);
        if (name is null)
        {
            StatusText.Text = "名称不能为空，且不能包含路径或非法文件名字符";
            PatchNameText.Focus();
            return;
        }
        MovePatch(file, name, file.IsEnabled, "已重命名");
    }

    private void PatchEnabledChanged(object sender, RoutedEventArgs args)
    {
        if (sender is not System.Windows.Controls.CheckBox { Tag: RuntimePatchFile file } checkBox)
            return;
        var enabled = checkBox.IsChecked == true;
        if (enabled == file.IsEnabled) return;
        if (!MovePatch(file, file.Name, enabled, enabled ? "补丁已启用" : "补丁已停用"))
            checkBox.IsChecked = file.IsEnabled;
    }

    private bool MovePatch(RuntimePatchFile file, string name, bool enabled, string message)
    {
        var target = RuntimePatchFile.BuildPath(_runtimeDirectory, name, enabled);
        if (file.FullName.Equals(target, StringComparison.Ordinal))
        {
            StatusText.Text = "名称没有变化";
            return true;
        }
        var counterpart = RuntimePatchFile.BuildPath(_runtimeDirectory, name, !enabled);
        var targetTaken = File.Exists(target) &&
            !file.FullName.Equals(target, StringComparison.OrdinalIgnoreCase);
        var counterpartTaken = File.Exists(counterpart) &&
            !file.FullName.Equals(counterpart, StringComparison.OrdinalIgnoreCase);
        if (targetTaken || counterpartTaken)
        {
            StatusText.Text = $"已存在同名补丁: {name}";
            return false;
        }

        try
        {
            if (file.FullName.Equals(target, StringComparison.OrdinalIgnoreCase))
            {
                var temporary = file.FullName + ".rename." + Guid.NewGuid().ToString("N");
                File.Move(file.FullName, temporary);
                try
                {
                    File.Move(temporary, target);
                }
                catch
                {
                    File.Move(temporary, file.FullName);
                    throw;
                }
            }
            else
            {
                File.Move(file.FullName, target);
            }

            if (_selectedPath?.Equals(file.FullName, StringComparison.OrdinalIgnoreCase) == true)
            {
                _selectedPath = target;
                SelectedFileText.Text = target;
                PatchNameText.Text = name;
            }
            RefreshFiles();
            StatusText.Text = message;
            return true;
        }
        catch (Exception exception)
        {
            StatusText.Text = $"修改补丁失败: {exception.Message}";
            return false;
        }
    }

    private static string? NormalizePatchName(string input)
    {
        var name = input.Trim();
        if (name.EndsWith(".ini.disabled", StringComparison.OrdinalIgnoreCase))
            name = name[..^".ini.disabled".Length];
        else if (name.EndsWith(".ini", StringComparison.OrdinalIgnoreCase))
            name = name[..^".ini".Length];
        name = name.Trim();
        if (name.Length == 0 || name is "." or ".." ||
            name.EndsWith(' ') || name.EndsWith('.') ||
            name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            return null;
        return name;
    }

    private void DeletePatch(object sender, RoutedEventArgs args)
    {
        if (_selectedPath is null) return;
        if (MessageBox.Show("删除当前补丁文件？", "RA2Hook Runtime",
                MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        try
        {
            File.Delete(_selectedPath);
            _editorDirty = false;
            _selectedPath = null;
            RefreshFiles();
        }
        catch (Exception exception)
        {
            StatusText.Text = $"删除失败: {exception.Message}";
        }
    }

    private async void ApplyPatch(object sender, RoutedEventArgs args) => await SendCommandAsync("RELOAD");
    private async void RollbackPatch(object sender, RoutedEventArgs args) => await SendCommandAsync("ROLLBACK");

    private async void AutoApplyChanged(object sender, RoutedEventArgs args)
    {
        if (_syncingAuto) return;
        await SendCommandAsync(AutoApplyCheck.IsChecked == true ? "AUTO 1" : "AUTO 0");
    }

    private async Task SendCommandAsync(string command)
    {
        try
        {
            var rows = await _pipe.SendAsync(command);
            var error = rows.FirstOrDefault(row => row.Length > 1 && row[0] == "ERROR");
            StatusText.Text = error is null ? "命令已发送" : error[1];
            await PollAsync();
        }
        catch (Exception exception)
        {
            ShowConnectionFailure(exception, "命令失败");
        }
    }

    private async Task PollAsync()
    {
        if (_polling) return;
        _polling = true;
        var connected = false;
        try
        {
            var rows = await _pipe.SendAsync("STATUS");
            var status = PipeClient.ParseStatus(rows);
            if (status is null) throw new IOException("状态响应无效");
            connected = true;
            ConnectionText.Text = status.Enabled ? "已连接" : "已连接，运行时未启用";
            ConnectionText.Foreground = new System.Windows.Media.SolidColorBrush(
                status.Enabled ? System.Windows.Media.Colors.ForestGreen :
                                 System.Windows.Media.Colors.DarkOrange);
            SessionText.Text = $"{status.Mode}  |  {(status.SinglePlayer ? "单机" : "禁止写入")}";
            _syncingAuto = true;
            AutoApplyCheck.IsChecked = status.AutoApply;
            _syncingAuto = false;
            StatusText.Text = status.Message;
            KeyCountText.Text = $"代数 {status.Generation}  已应用 {status.AppliedKeys}  拒绝 {status.RejectedKeys}";
            if (status.Generation != _lastGeneration || status.Message != _lastMessage ||
                _lastGeneration < 0)
            {
                var inspect = await _pipe.SendAsync("INSPECT");
                _items.Clear();
                foreach (var item in PipeClient.ParseItems(inspect)) _items.Add(item);
                _lastGeneration = status.Generation;
                _lastMessage = status.Message;
            }
        }
        catch (Exception exception)
        {
            _syncingAuto = false;
            ShowConnectionFailure(exception, "等待游戏连接");
            ApplyPatchButtonState(false);
        }
        ApplyPatchButtonState(connected);
        _polling = false;
    }

    private void ShowConnectionFailure(Exception exception, string context)
    {
        var permissionDenied = exception is UnauthorizedAccessException ||
            exception.Message.Contains("denied", StringComparison.OrdinalIgnoreCase) ||
            exception.Message.Contains("拒绝", StringComparison.OrdinalIgnoreCase);
        var runtimeDisabled = IniDocument.ReadRuntimeEnabled(_gameRoot) == false;
        ConnectionText.Text = permissionDenied ? "连接权限不足" :
                              runtimeDisabled ? "运行时未启用" : "未连接";
        ConnectionText.Foreground = new System.Windows.Media.SolidColorBrush(
            permissionDenied ? System.Windows.Media.Colors.Firebrick :
            runtimeDisabled ? System.Windows.Media.Colors.DarkOrange :
                              System.Windows.Media.Colors.Gray);
        StatusText.Text = permissionDenied
            ? "游戏与 UI 权限不同；新版本 DLL 会允许本机 UI 连接"
            : runtimeDisabled
            ? "请将 ra2hook.ini 的 [Runtime] Enabled 改为 yes，然后重启游戏"
            : context;
        LogText.Text = exception.Message;
    }

    private void ApplyPatchButtonState(bool connected)
    {
        var canApply = connected && _gameRoot.Length > 0;
        AutoApplyCheck.IsEnabled = canApply;
        ApplyButton.IsEnabled = canApply;
        RollbackButton.IsEnabled = canApply;
    }

    private void ResultFilterChanged(object sender, System.Windows.Controls.TextChangedEventArgs args)
    {
        var filter = ResultFilterText.Text.Trim();
        var view = CollectionViewSource.GetDefaultView(_items);
        view.Filter = value => value is RuntimeItem item &&
            (filter.Length == 0 || item.Section.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
             item.Key.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
             item.Safety.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
             item.Result.Contains(filter, StringComparison.OrdinalIgnoreCase));
        view.Refresh();
    }

    private async void BrowseRoot(object sender, RoutedEventArgs args)
    {
        if (_editorDirty && MessageBox.Show("当前补丁尚未保存，确定切换游戏目录？",
                "RA2Hook Runtime", MessageBoxButton.OKCancel,
                MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        var dialog = new OpenFolderDialog { Title = "选择红警2游戏目录" };
        if (dialog.ShowDialog() != true) return;
        _editorDirty = false;
        _selectedPath = null;
        GameRootText.Text = dialog.FolderName;
        ConfigureRoot();
        await RefreshDumpCatalogAsync();
    }

    private async void RefreshDumpClicked(object sender, RoutedEventArgs args) =>
        await RefreshDumpCatalogAsync();

    private async Task RefreshDumpCatalogAsync()
    {
        if (_loadingDump || string.IsNullOrWhiteSpace(_gameRoot)) return;
        _loadingDump = true;
        ExtractUnitButton.IsEnabled = false;
        OpenUnitOutputButton.IsEnabled = false;
        DumpSourceText.Text = "正在读取 Dump...";
        try
        {
            var units = await _dumpExtractor.LoadAsync(_gameRoot);
            _dumpUnits.Clear();
            foreach (var unit in units) _dumpUnits.Add(unit);
            DumpSourceText.Text = _dumpExtractor.DumpRoot;
            ApplyUnitFilter();
            DumpUnitsList.SelectedItem = CollectionViewSource.GetDefaultView(_dumpUnits)
                .Cast<DumpUnitInfo>().FirstOrDefault();
            StatusText.Text = $"Dump 扫描完成，共 {units.Count} 个已注册单位";
        }
        catch (Exception exception)
        {
            _dumpUnits.Clear();
            DumpSourceText.Text = exception is FileNotFoundException fileNotFound
                ? fileNotFound.FileName ?? "Dump INI 不完整"
                : "Dump 读取失败";
            UnitCountText.Text = "0";
            ClearUnitPreview(exception.Message);
            StatusText.Text = exception.Message;
        }
        finally
        {
            _loadingDump = false;
        }
    }

    private void UnitFilterChanged(object sender, RoutedEventArgs args) => ApplyUnitFilter();

    private void ApplyUnitFilter()
    {
        if (DumpUnitsList is null || UnitCategoryFilter is null || UnitSearchText is null) return;
        var category = (UnitCategoryFilter.SelectedItem as System.Windows.Controls.ComboBoxItem)?
            .Tag?.ToString() ?? string.Empty;
        var search = UnitSearchText.Text.Trim();
        var view = CollectionViewSource.GetDefaultView(_dumpUnits);
        view.Filter = value => value is DumpUnitInfo unit &&
            (category.Length == 0 || unit.CategoryKey.Equals(category,
                StringComparison.OrdinalIgnoreCase)) &&
            (search.Length == 0 || unit.Id.Contains(search, StringComparison.OrdinalIgnoreCase));
        view.Refresh();
        UnitCountText.Text = view.Cast<object>().Count().ToString();
    }

    private void DumpUnitSelected(object sender, System.Windows.Controls.SelectionChangedEventArgs args)
    {
        if (DumpUnitsList.SelectedItem is not DumpUnitInfo unit)
        {
            ClearUnitPreview("请选择一个已注册单位");
            return;
        }

        try
        {
            var preview = _dumpExtractor.Preview(unit);
            SelectedUnitText.Text = $"{unit.Id}  ·  {unit.Category}";
            UnitRegistryText.Text = $"[{unit.RegistrySection}]  {unit.RegistrationKey}={unit.Id}";
            UnitRulesCountText.Text = $"{preview.RulesSections.Count} 个关联段";
            UnitArtCountText.Text = $"{preview.ArtSections.Count} 个段  /  {preview.MaterialFiles} 个文件";
            UnitOutputPathText.Text = preview.OutputDirectory;
            UnitPreviewText.Text =
                "rules.ini\r\n" + string.Join(", ", preview.RulesSections.Select(name => $"[{name}]")) +
                "\r\n\r\nart.ini\r\n" +
                (preview.ArtSections.Count == 0
                    ? "（没有匹配的 Art 段）"
                    : string.Join(", ", preview.ArtSections.Select(name => $"[{name}]"))) +
                $"\r\n\r\n素材文件\r\n{preview.MaterialFiles} 个";
            ExtractUnitButton.IsEnabled = unit.HasRules && preview.MaterialFiles > 0;
            _lastUnitOutput = Directory.Exists(preview.OutputDirectory)
                ? preview.OutputDirectory
                : null;
            OpenUnitOutputButton.IsEnabled = _lastUnitOutput is not null;
        }
        catch (Exception exception)
        {
            ClearUnitPreview(exception.Message);
            StatusText.Text = exception.Message;
        }
    }

    private void ClearUnitPreview(string message)
    {
        SelectedUnitText.Text = "未选择单位";
        UnitRegistryText.Text = string.Empty;
        UnitRulesCountText.Text = string.Empty;
        UnitArtCountText.Text = string.Empty;
        UnitOutputPathText.Text = string.Empty;
        UnitPreviewText.Text = message;
        ExtractUnitButton.IsEnabled = false;
        OpenUnitOutputButton.IsEnabled = false;
        _lastUnitOutput = null;
    }

    private async void ExtractUnit(object sender, RoutedEventArgs args)
    {
        if (DumpUnitsList.SelectedItem is not DumpUnitInfo unit) return;
        ExtractUnitButton.IsEnabled = false;
        try
        {
            var result = await _dumpExtractor.ExtractAsync(
                unit, OverwriteUnitCheck.IsChecked == true);
            _lastUnitOutput = result.OutputDirectory;
            UnitOutputPathText.Text = result.OutputDirectory;
            OpenUnitOutputButton.IsEnabled = true;
            StatusText.Text =
                $"已提取 {unit.Id}: rules {result.RulesSections} 段，art {result.ArtSections} 段，素材 {result.MaterialFiles} 个";
        }
        catch (Exception exception)
        {
            StatusText.Text = $"提取失败: {exception.Message}";
        }
        finally
        {
            ExtractUnitButton.IsEnabled = DumpUnitsList.SelectedItem is DumpUnitInfo selected &&
                                          selected.HasRules && selected.MaterialFiles > 0;
        }
    }

    private void OpenUnitOutput(object sender, RoutedEventArgs args)
    {
        if (_lastUnitOutput is null || !Directory.Exists(_lastUnitOutput)) return;
        try
        {
            Process.Start(new ProcessStartInfo(_lastUnitOutput) { UseShellExecute = true });
        }
        catch (Exception exception)
        {
            StatusText.Text = $"打开目录失败: {exception.Message}";
        }
    }
}
