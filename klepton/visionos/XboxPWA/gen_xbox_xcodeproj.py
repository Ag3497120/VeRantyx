#!/usr/bin/env python3
"""XboxPWA.xcodeproj generator — the smallest project that builds the Xbox
Cloud Gaming window: one Swift file, one asset catalog, one plist. Kept apart
from gen_xcodeproj.py because that generator's whole shape is the Klepton
runtime (guest frameworks, ANGLE, MoltenVK) and this app deliberately has none
of it."""
import os, re, subprocess

def detect_team():
    if os.environ.get("KLEPTON_TEAM"):
        return os.environ["KLEPTON_TEAM"]
    try:
        pem = subprocess.run(["security", "find-certificate", "-c", "Apple Development",
                              "-p"], capture_output=True, text=True).stdout
        subj = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                              input=pem, capture_output=True, text=True).stdout
        m = re.search(r"OU\s*=\s*([A-Z0-9]{10})", subj)
        if m:
            return m.group(1)
    except Exception:
        pass
    return ""

TEAM = detect_team()
scope = os.environ.get("KLEPTON_BUNDLE_SCOPE", os.environ.get("USER", ""))
scope = re.sub(r"[^A-Za-z0-9-]", "-", scope).strip("-").lower()
BUNDLE = (f"{scope}." if scope else "") + "dev.klepton.target.xbox"

def oid(tag):
    return ("XB0X" + tag).upper().ljust(24, "0")[:24]

PROJ, TGT, PROD = oid("PROJ"), oid("TGT"), oid("PROD")
GROOT, GPROD = oid("GROOT"), oid("GPROD")
BPSRC, BPRES = oid("BPSRC"), oid("BPRES")
FSW, BSW = oid("FSW"), oid("BSW")
FAS, BAS = oid("FAS"), oid("BAS")
FPL = oid("FPL")
CLP, CLT = oid("CLP"), oid("CLT")
CPD, CPR, CTD, CTR = oid("CPD"), oid("CPR"), oid("CTD"), oid("CTR")

COMMON = f"""
				CODE_SIGN_STYLE = Automatic;
				DEVELOPMENT_TEAM = {TEAM};
				GENERATE_INFOPLIST_FILE = YES;
				INFOPLIST_FILE = Info.plist;
				ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
				INFOPLIST_KEY_CFBundleDisplayName = "Xbox";
				INFOPLIST_KEY_GCSupportsControllerUserInteraction = NO;
				INFOPLIST_KEY_UIApplicationSceneManifest_Generation = YES;
				PRODUCT_BUNDLE_IDENTIFIER = "{BUNDLE}";
				PRODUCT_NAME = XboxPWA;
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				ALWAYS_SEARCH_USER_PATHS = NO;
				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 7;
				XROS_DEPLOYMENT_TARGET = 2.0;
"""

pbx = f"""// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{
	}};
	objectVersion = 56;
	objects = {{

/* Begin PBXBuildFile section */
		{BSW} /* XboxApp.swift in Sources */ = {{isa = PBXBuildFile; fileRef = {FSW}; }};
		{BAS} /* Assets.xcassets in Resources */ = {{isa = PBXBuildFile; fileRef = {FAS}; }};
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		{PROD} /* XboxPWA.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = XboxPWA.app; sourceTree = BUILT_PRODUCTS_DIR; }};
		{FSW} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = XboxApp.swift; sourceTree = "<group>"; }};
		{FAS} = {{isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = Assets.xcassets; sourceTree = "<group>"; }};
		{FPL} = {{isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = "<group>"; }};
/* End PBXFileReference section */

/* Begin PBXGroup section */
		{GROOT} = {{
			isa = PBXGroup;
			children = (
				{FSW},
				{FAS},
				{FPL},
				{GPROD},
			);
			sourceTree = "<group>";
		}};
		{GPROD} /* Products */ = {{
			isa = PBXGroup;
			children = (
				{PROD},
			);
			name = Products;
			sourceTree = "<group>";
		}};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		{TGT} /* XboxPWA */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {CLT};
			buildPhases = (
				{BPSRC},
				{BPRES},
			);
			buildRules = (
			);
			dependencies = (
			);
			name = XboxPWA;
			productName = XboxPWA;
			productReference = {PROD};
			productType = "com.apple.product-type.application";
		}};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		{PROJ} /* Project object */ = {{
			isa = PBXProject;
			attributes = {{
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 1500;
				LastUpgradeCheck = 1500;
			}};
			buildConfigurationList = {CLP};
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (
				en,
				Base,
			);
			mainGroup = {GROOT};
			productRefGroup = {GPROD};
			projectDirPath = "";
			projectRoot = "";
			targets = (
				{TGT},
			);
		}};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		{BPRES} = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{BAS},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXResourcesBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		{BPSRC} = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{BSW},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		{CPD} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				SWIFT_OPTIMIZATION_LEVEL = "-Onone";
			}};
			name = Debug;
		}};
		{CPR} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
			}};
			name = Release;
		}};
		{CTD} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Debug;
		}};
		{CTR} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Release;
		}};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		{CLP} /* Build configuration list for PBXProject */ = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{CPD},
				{CPR},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Debug;
		}};
		{CLT} /* Build configuration list for PBXNativeTarget */ = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{CTD},
				{CTR},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Debug;
		}};
/* End XCConfigurationList section */
	}};
	rootObject = {PROJ};
}}
"""

os.makedirs("XboxPWA.xcodeproj", exist_ok=True)
with open("XboxPWA.xcodeproj/project.pbxproj", "w") as f:
    f.write(pbx)
print(f"XboxPWA.xcodeproj written  (bundle {BUNDLE}, team {TEAM or 'NOT DETECTED - set KLEPTON_TEAM'})")
