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
    private readonly ObservableCollection<FileInfo> _files = new();
    private readonly ObservableCollection<RuntimeItem> _items = new();
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

    public MainWindow()
    {
        InitializeComponent();
        FilesList.ItemsSource = _files;
        RuntimeGrid.ItemsSource = _items;
        _pollTimer.Tick += async (_, _) => await PollAsync();
        Loaded += async (_, _) =>
        {
            GameRootText.Text = DiscoverGameRoot();
            ConfigureRoot();
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
        var candidates = new[]
        {
            AppContext.BaseDirectory,
            Directory.GetCurrentDirectory(),
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", ".."))
        };
        return candidates.FirstOrDefault(path => File.Exists(Path.Combine(path, "ra2hook.ini")) ||
                                                  File.Exists(Path.Combine(path, "gamemd.exe"))) ??
               AppContext.BaseDirectory;
    }

    private void ConfigureRoot()
    {
        _gameRoot = Path.GetFullPath(GameRootText.Text.Trim());
        _runtimeDirectory = IniDocument.ResolveRuntimeDirectory(_gameRoot);
        RuntimePathText.Text = _runtimeDirectory;
        _watcher?.Dispose();
        Directory.CreateDirectory(_runtimeDirectory);
        _watcher = new FileSystemWatcher(_runtimeDirectory, "*.ini")
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
        foreach (var path in Directory.EnumerateFiles(_runtimeDirectory, "*.ini")
                     .OrderBy(value => value, StringComparer.OrdinalIgnoreCase))
            _files.Add(new FileInfo(path));
        var next = _files.FirstOrDefault(value => value.FullName.Equals(selected,
            StringComparison.OrdinalIgnoreCase)) ?? _files.FirstOrDefault();
        FilesList.SelectedItem = next;
    }

    private async void PatchSelected(object sender, System.Windows.Controls.SelectionChangedEventArgs args)
    {
        if (_suppressSelection) return;
        if (FilesList.SelectedItem is not FileInfo file) return;
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
            var path = Path.Combine(_runtimeDirectory, $"patch-{timestamp}.ini");
            var suffix = 1;
            while (File.Exists(path))
                path = Path.Combine(_runtimeDirectory,
                    $"patch-{timestamp}-{suffix++}.ini");
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
            ConnectionText.Text = "未连接";
            StatusText.Text = $"命令失败: {exception.Message}";
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
                _items.Count == 0)
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
            ConnectionText.Text = "未连接";
            ConnectionText.Foreground = new System.Windows.Media.SolidColorBrush(
                System.Windows.Media.Colors.Gray);
            ApplyPatchButtonState(false);
            StatusText.Text = "等待游戏连接";
            LogText.Text = exception.Message;
        }
        ApplyPatchButtonState(connected);
        _polling = false;
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

    private void BrowseRoot(object sender, RoutedEventArgs args)
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
    }
}
