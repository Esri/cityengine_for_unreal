/* Copyright 2024 Esri
 *
 * Licensed under the Apache License Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

using System.IO;
using System;
using System.Diagnostics;
using System.Security.Cryptography;
using UnrealBuildTool;
using System.ComponentModel.Design;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using EpicGames.Core;

public class PRT : ModuleRules
{
	private readonly bool Debug;

	private const string PrtCoreDllName = "com.esri.prt.core.dll";

	private static readonly List<string> FilteredExtensionLibraries = new List<string>() { "DatasmithSDK.dll", "FreeImage317.dll", "com.esri.prt.unreal.dll" };

	public PRT(ReadOnlyTargetRules Target) : base(Target)
	{
		// Debug print only enabled when plugin is installed directly into project (not in Engine)
		Debug = !PluginDirectory.EndsWith(Path.Combine("Plugins", "Marketplace", "Vitruvio"));
		
		bUseRTTI = true;
		bEnableExceptions = true;
		Type = ModuleType.External;

		AbstractPlatform Platform;
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			Platform = new WindowsPlatform(Debug);
		}
		else
		{
			throw new System.PlatformNotSupportedException();
		}

		string VersionPath = Path.Combine(ModuleDirectory, "PRT.version.json");
		ExternalDependencies.Add(VersionPath);
		JsonObject Version = JsonObject.Read(new FileReference(VersionPath));
		if (!Version.TryGetIntegerField("major", out int PrtMajor) || PrtMajor <= 0 ||
			!Version.TryGetIntegerField("minor", out int PrtMinor) || PrtMinor < 0 ||
			!Version.TryGetIntegerField("build", out int PrtBuild) || PrtBuild <= 0 ||
			!Version.TryGetStringField("toolchain", out string PrtToolchain) ||
			!Regex.IsMatch(PrtToolchain, @"^win[0-9]+-vc[0-9]{4}-x86_64-rel-opt$") ||
			!Version.TryGetStringField("sha256", out string PrtSha256) ||
			!Regex.IsMatch(PrtSha256, @"^[0-9a-fA-F]{64}$"))
		{
			throw new BuildException($"Invalid PRT SDK metadata in {VersionPath}: expected major/minor/build, a Windows x64 release toolchain, and a SHA-256 digest.");
		}

		string LibDir = Path.Combine(ModuleDirectory, "lib", Platform.Name, "Release");
		string BinDir = Path.Combine(ModuleDirectory, "bin", Platform.Name, "Release");
		string IncludeDir = Path.Combine(ModuleDirectory, "include");

		// 1. Check if prt is already available and has correct version, otherwise download from official github repo
		bool PrtInstalled = Directory.Exists(LibDir) && Directory.Exists(BinDir);
		
		string PrtCorePath = Path.Combine(BinDir, PrtCoreDllName);
		bool PrtCoreExists = File.Exists(PrtCorePath);
		bool PrtVersionMatch = PrtCoreExists && CheckDllVersion(Platform, PrtCorePath, PrtMajor, PrtMinor, PrtBuild);

		if (!PrtInstalled || !PrtVersionMatch)
		{

			string PrtUrl = "https://github.com/Esri/esri-cityengine-sdk/releases/download";
			string PrtVersion = string.Format("{0}.{1}.{2}", PrtMajor, PrtMinor, PrtBuild);

			string PrtLibName = string.Format("esri_ce_sdk-{0}-{1}", PrtVersion, PrtToolchain);
			string PrtLibZipFile = PrtLibName + ".zip";
			string PrtDownloadUrl = Path.Combine(PrtUrl, PrtVersion, PrtLibZipFile);

			try
			{
				if (Directory.Exists(LibDir)) Directory.Delete(LibDir, true);
				if (Directory.Exists(BinDir)) Directory.Delete(BinDir, true);
				if (Directory.Exists(IncludeDir)) Directory.Delete(IncludeDir, true);

				if (Debug)
				{
					if (!PrtInstalled) Console.WriteLine("PRT not found");
					Console.WriteLine("Updating PRT");
				}

				if (Debug) System.Console.WriteLine("Downloading " + PrtDownloadUrl + "...");
				
				Platform.DownloadFile(PrtDownloadUrl, Path.Combine(ModuleDirectory, PrtLibZipFile));

				if (Debug) System.Console.WriteLine("Extracting " + PrtLibZipFile + "...");

				Platform.ZipExtractor.Unzip(ModuleDirectory, PrtLibZipFile, PrtLibName);

				Directory.CreateDirectory(LibDir);
				Directory.CreateDirectory(BinDir);
				Copy(Path.Combine(ModuleDirectory, PrtLibName, "lib"), Path.Combine(ModuleDirectory, LibDir), FilteredExtensionLibraries);
				Copy(Path.Combine(ModuleDirectory, PrtLibName, "bin"), Path.Combine(ModuleDirectory, BinDir));
				Copy(Path.Combine(ModuleDirectory, PrtLibName, "include"), Path.Combine(ModuleDirectory, "include"));
			}
			finally
			{
				Directory.Delete(Path.Combine(ModuleDirectory, PrtLibName), true);
				File.Delete(Path.Combine(ModuleDirectory, PrtLibZipFile));
			}
		}
		else if (Debug)
		{
			Console.WriteLine("PRT found");
		}
		
		// 2. Copy libraries to module binaries directory and add dependencies
		string ModuleBinariesDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../..", "Binaries", Platform.Name));

		if (Debug)
		{
			System.Console.WriteLine("PRT Source Lib Dir: " + LibDir);
			System.Console.WriteLine("PRT Source Bin Dir: " + BinDir);
			System.Console.WriteLine("PRT Source Include Dir: " + IncludeDir);
			System.Console.WriteLine("Module Binaries Dir: " + ModuleBinariesDir);
		}

		Directory.CreateDirectory(ModuleBinariesDir);
		PublicRuntimeLibraryPaths.Add(ModuleBinariesDir);

		// Add PRT core libraries
		if (Debug) Console.WriteLine("Adding PRT core libraries");
		foreach (string FilePath in Directory.GetFiles(BinDir))
		{
			string LibraryName = Path.GetFileName(FilePath);

			Platform.AddPrtCoreLibrary(FilePath, LibraryName, this);
		}
		
		// Add extension libraries as run-time dependencies
		if (Debug) Console.WriteLine("Adding PRT extension libraries");
		Platform.AddExtensionLibraries(LibDir, this);

		// Add include search path
		if (Debug) Console.WriteLine("Adding include search path " + IncludeDir);
		PublicSystemIncludePaths.Add(IncludeDir);
		
		// Add prt version defines
		PublicDefinitions.Add($"PRT_VERSION_MAJOR={PrtMajor}");
		PublicDefinitions.Add($"PRT_VERSION_MINOR={PrtMinor}");
	}

	void Copy(string SrcDir, string DstDir, List<string> Filter = null)
	{
		Directory.CreateDirectory(DstDir);

		foreach (string CopyFile in Directory.GetFiles(SrcDir))
		{
			if (Filter == null || !Filter.Contains(Path.GetFileName(CopyFile)))
			{
				File.Copy(CopyFile, Path.Combine(DstDir, Path.GetFileName(CopyFile)));
			}
		}

		foreach (string Dir in Directory.GetDirectories(SrcDir))
		{
			Copy(Dir, Path.Combine(DstDir, Path.GetFileName(Dir)));
		}
	}

	private bool CheckDllVersion(AbstractPlatform Platform, string DllPath, int Major, int Minor, int Build)
	{
		string FileVersion = Platform.GetFileVersionInfo(ModuleDirectory, DllPath);
		string[] BuildVersions = FileVersion.Split(' ');
		string ProductVersion = BuildVersions[0];
		string[] ProductVersions = ProductVersion.Split('.');
		int FileMajor = int.Parse(ProductVersions[0]);
		int FileMinor = int.Parse(ProductVersions[1]);
		int DllBuild = int.Parse(BuildVersions[BuildVersions.Length - 1]);
		
		bool Match = FileMajor == Major && FileMinor == Minor && DllBuild == Build;
		if (Debug && !Match)
		{
			Console.WriteLine(string.Format("Version {0}.{1}.{2} of \"{3}\" does not match expected version of Build file {4}.{5}.{6}",
				FileMajor, FileMinor, DllBuild, Path.GetFileName(DllPath), Major, Minor, Build));
		}
		return Match;
	}

	private abstract class AbstractZipExtractor
	{
		public void Unzip(string WorkingDir, string ZipFile, string Destination)
		{
			string ExpandedArguments = string.Format(Arguments, ZipFile, Destination);

			ProcessStartInfo ProcStartInfo = new System.Diagnostics.ProcessStartInfo(Command, ExpandedArguments)
			{
				WorkingDirectory = WorkingDir,
				UseShellExecute = false,
				CreateNoWindow = true
			};

			System.Diagnostics.Process UnzipProcess = new System.Diagnostics.Process
			{
				StartInfo = ProcStartInfo,
				EnableRaisingEvents = true
			};
			UnzipProcess.Start();
			UnzipProcess.WaitForExit();
		}

		public abstract string Command { get; }
		public abstract string Arguments { get; }
	}

	private class WindowsZipExtractor : AbstractZipExtractor
	{
		public override string Command { get { return "cmd"; } }

		public override string Arguments
		{
			get
			{
				return "/c PowerShell -Command \" & Expand-Archive -Path {0} -DestinationPath {1}\"";
			}
		}
	}

	private abstract class AbstractPlatform
	{
		public abstract AbstractZipExtractor ZipExtractor { get; }

		public abstract string Name { get; }
		public abstract string DynamicLibExtension { get; }

		protected bool Debug;
		public AbstractPlatform(bool Debug)
		{
			this.Debug = Debug;
		}

		public virtual void AddExtensionLibraries(string SourceFolder, ModuleRules Rules)
		{
			foreach (string Dir in Directory.GetDirectories(SourceFolder))
			{
				AddExtensionLibraries(Dir, Rules);
			}
			
			foreach (string FilePath in Directory.GetFiles(SourceFolder))
			{
				Rules.RuntimeDependencies.Add(FilePath);
			}
		}

		public virtual void AddPrtCoreLibrary(string LibraryPath, string LibraryName, ModuleRules Rules)
		{
			if (Path.GetExtension(LibraryName) == DynamicLibExtension)
			{
				if (Debug) Console.WriteLine("Adding Runtime Library " + LibraryName);

				Rules.RuntimeDependencies.Add(LibraryPath);
				Rules.PublicDelayLoadDLLs.Add(LibraryName);
			}
		}
		public abstract string GetFileVersionInfo(string WorkingDir, string Path);
		public abstract void DownloadFile(string Url, string Destination);
	}

	private class WindowsPlatform : AbstractPlatform
	{
		public override AbstractZipExtractor ZipExtractor { get { return new WindowsZipExtractor(); } }

		public override string Name { get { return "Win64"; } }
		public override string DynamicLibExtension { get { return ".dll"; } }
		
		public WindowsPlatform(bool Debug) : base(Debug)
		{
		}
		
		public override void AddPrtCoreLibrary(string LibraryPath, string LibraryName, ModuleRules Rules)
		{
			base.AddPrtCoreLibrary(LibraryPath, LibraryName, Rules);

			if (Path.GetExtension(LibraryPath) == ".lib")
			{
				if (Debug) Console.WriteLine("Adding Public Additional Library " + LibraryName);

				Rules.PublicAdditionalLibraries.Add(LibraryPath);
			}
		}
		

		public override string GetFileVersionInfo(string WorkingDir, string Path)
		{
			string GetFileInfoCommand = "/c PowerShell -Command \"(Get-Item \'{0}\').VersionInfo.FileVersion\"";
			string ExpandedFileInfoCommand = string.Format(GetFileInfoCommand, Path);

			ProcessStartInfo ProcStartInfo = new System.Diagnostics.ProcessStartInfo("cmd", ExpandedFileInfoCommand)
			{
				WorkingDirectory = WorkingDir,
				UseShellExecute = false,
				CreateNoWindow = true,
				RedirectStandardOutput = true
			};

			Process FileVersionProcess = new Process
			{
				StartInfo = ProcStartInfo,
				EnableRaisingEvents = true
			};
			FileVersionProcess.Start();
			
			string Output = FileVersionProcess.StandardOutput.ReadToEnd();
			FileVersionProcess.WaitForExit();

			return Output;
		}
		
		public override void DownloadFile(string Url, string Destination)
		{
			string DownloadFileCommand = "/c PowerShell -Command \"Invoke-WebRequest -Uri {0} -OutFile '{1}'\"";
			string ExpandedDownloadFileCommand = string.Format(DownloadFileCommand, Url, Destination);

			ProcessStartInfo ProcStartInfo = new System.Diagnostics.ProcessStartInfo("cmd", ExpandedDownloadFileCommand)
			{
				UseShellExecute = false,
				CreateNoWindow = true,
			};

			Process FileVersionProcess = new Process
			{
				StartInfo = ProcStartInfo,
				EnableRaisingEvents = true
			};
			FileVersionProcess.Start();
			FileVersionProcess.WaitForExit();
		}
	}
}
