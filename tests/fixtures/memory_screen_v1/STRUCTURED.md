# Structured fixture
<!-- AGENT_MEMORY_V1_BEGIN -->
{
  "schema": "agent_memory_v2",
  "items": {
    "drive.product": {
      "kind": "fact",
      "status": "active",
      "value": "AX_M7 drive controller has product code DRIVE_S5.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "valve/control~pid": {
      "kind": "fact",
      "status": "active",
      "value": "Control routine pump_pid_K42 runs with a 35 ms sample period.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "alias.mercury": {
      "kind": "fact",
      "status": "active",
      "value": "MSR is the alias of MercurySwitch service.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "mercury.location": {
      "kind": "fact",
      "status": "active",
      "value": "MercurySwitch service is hosted in the Olen datacenter.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "mx54.vendor": {
      "kind": "fact",
      "status": "active",
      "value": "MX_54 vendor is PlatinWorks (铂青工坊).",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "platin.city": {
      "kind": "fact",
      "status": "active",
      "value": "PlatinWorks (铂青工坊) is based in 鹤岑市 (Hecen).",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "delta.supplier": {
      "kind": "fact",
      "status": "active",
      "value": "DeltaPier pumps are supplied by LarchFoundry.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "larch.city": {
      "kind": "fact",
      "status": "active",
      "value": "LarchFoundry headquarters is in Alderhaven.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "project.service": {
      "kind": "fact",
      "status": "active",
      "value": "Project Halcyon uses the BeaconLedger service.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "beacon.owner": {
      "kind": "fact",
      "status": "active",
      "value": "BeaconLedger service is owned by the CedarOperations team.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "certificate.verdict": {
      "kind": "fact",
      "status": "active",
      "value": "Certificate CERT_200 records a pressure test at 8.2 MPa and verdict accepted.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "spectra.board": {
      "kind": "fact",
      "status": "active",
      "value": "SpectraUnit sensor uses board BOARD_S17.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "board.processor": {
      "kind": "fact",
      "status": "active",
      "value": "BOARD_S17 is fitted with processor CPU_RV32.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "cpu.arch": {
      "kind": "fact",
      "status": "active",
      "value": "Processor CPU_RV32 implements the RV32IMC architecture.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "sop.action": {
      "kind": "fact",
      "status": "active",
      "value": "Quality SOP QL_82 runs action trace_emit.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "tool.binding": {
      "kind": "fact",
      "status": "active",
      "value": "Action trace_emit uses tool AuditIngress.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "tool.transport": {
      "kind": "fact",
      "status": "active",
      "value": "AuditIngress tool sends data using HTTPS POST.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "terra.release": {
      "kind": "fact",
      "status": "active",
      "value": "TerraLine approved firmware is terra_now_v3, signed by BirchKey.",
      "updated_utc": "2026-11-01T00:00:00Z",
      "versions": [
        {
          "kind": "fact",
          "status": "active",
          "value": "TerraLine firmware terra_old_v1 was signed by WillowKey.",
          "updated_utc": "2026-08-01T00:00:00Z"
        }
      ]
    },
    "kappa.retired": {
      "kind": "fact",
      "status": "deprecated",
      "value": "Retired KappaPort endpoint uses protocol mqtt-v3.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "future.release": {
      "kind": "fact",
      "status": "active",
      "value": "Pylon future_release_9 is signed by FutureSigner.",
      "updated_utc": "2026-11-01T00:00:00Z",
      "valid_from": "9999-01-01T00:00:00Z"
    },
    "atlas.route.a": {
      "kind": "fact",
      "status": "active",
      "value": "AtlasRoute is assigned to NorthDepot; source ticket NORTH_11.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "atlas.route.b": {
      "kind": "fact",
      "status": "active",
      "value": "AtlasRoute is assigned to SouthDepot; source ticket SOUTH_12.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "青榆站.电机": {
      "kind": "fact",
      "status": "active",
      "value": "青榆站使用昆杉电机。",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "purchase.sop": {
      "kind": "fact",
      "status": "active",
      "value": "Purchase PO_L62 follows saved approval SOP PC_14.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "purchase.stage": {
      "kind": "fact",
      "status": "active",
      "value": "SOP PC_14 selects the expenditure check action expense_check.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "purchase.tool": {
      "kind": "fact",
      "status": "active",
      "value": "Action expense_check uses deterministic tool LedgerVerify.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.00": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_00 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_100.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.01": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_01 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_101.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.02": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_02 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_102.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.03": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_03 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_103.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.04": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_04 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_104.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.05": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_05 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_105.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.06": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_06 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_106.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.07": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_07 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_107.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.08": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_08 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_108.",
      "updated_utc": "2026-11-01T00:00:00Z"
    },
    "maintenance.note.09": {
      "kind": "fact",
      "status": "active",
      "value": "InspectionNode NODE_09 maintenance reference: review service route and supplier records before device replacement. Reference ticket MAINT_109.",
      "updated_utc": "2026-11-01T00:00:00Z"
    }
  }
}
<!-- AGENT_MEMORY_V1_END -->
