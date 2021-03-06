#include <sys/param.h>

#include <machine/intr_machdep.h>
#include <machine/msi_machdep.h>
#include <machine/msi_var.h>
#include <machine/md_var.h>

#include <machine_base/apic/apicreg.h>
#include <machine_base/apic/lapic.h>

/* MSI address */
#define	MSI_X86_ADDR_DEST		0x000ff000
#define	MSI_X86_ADDR_RH			0x00000008
# define MSI_X86_ADDR_RH_ON		0x00000008
# define MSI_X86_ADDR_RH_OFF		0x00000000
#define	MSI_X86_ADDR_DM			0x00000004
# define MSI_X86_ADDR_DM_PHYSICAL	0x00000000
# define MSI_X86_ADDR_DM_LOGICAL	0x00000004

/* MSI data */
#define	MSI_X86_DATA_TRGRMOD		IOART_TRGRMOD	/* Trigger mode. */
# define MSI_X86_DATA_TRGREDG		IOART_TRGREDG
# define MSI_X86_DATA_TRGRLVL		IOART_TRGRLVL
#define	MSI_X86_DATA_LEVEL		0x00004000	/* Polarity. */
# define MSI_X86_DATA_DEASSERT		0x00000000
# define MSI_X86_DATA_ASSERT		0x00004000
#define	MSI_X86_DATA_DELMOD		IOART_DELMOD	/* Delivery mode. */
# define MSI_X86_DATA_DELFIXED		IOART_DELFIXED
# define MSI_X86_DATA_DELLOPRI		IOART_DELLOPRI
# define MSI_X86_DATA_DELSMI		IOART_DELSMI
# define MSI_X86_DATA_DELNMI		IOART_DELNMI
# define MSI_X86_DATA_DELINIT		IOART_DELINIT
# define MSI_X86_DATA_DELEXINT		IOART_DELEXINT
#define	MSI_X86_DATA_INTVEC		IOART_INTVEC	/* Interrupt vector. */

#define	MSI_X86_ADDR(lapic_id) \
	(MSI_X86_ADDR_BASE | (lapic_id) << 12 | \
	 MSI_X86_ADDR_RH_OFF | MSI_X86_ADDR_DM_PHYSICAL)
#define	MSI_X86_DATA(vector) \
	(MSI_X86_DATA_TRGREDG | MSI_X86_DATA_DELFIXED | (vector))

extern inthand_t
	IDTVEC(msi_intr0),
	IDTVEC(msi_intr1),
	IDTVEC(msi_intr2),
	IDTVEC(msi_intr3),
	IDTVEC(msi_intr4),
	IDTVEC(msi_intr5),
	IDTVEC(msi_intr6),
	IDTVEC(msi_intr7),
	IDTVEC(msi_intr8),
	IDTVEC(msi_intr9),
	IDTVEC(msi_intr10),
	IDTVEC(msi_intr11),
	IDTVEC(msi_intr12),
	IDTVEC(msi_intr13),
	IDTVEC(msi_intr14),
	IDTVEC(msi_intr15),
	IDTVEC(msi_intr16),
	IDTVEC(msi_intr17),
	IDTVEC(msi_intr18),
	IDTVEC(msi_intr19),
	IDTVEC(msi_intr20),
	IDTVEC(msi_intr21),
	IDTVEC(msi_intr22),
	IDTVEC(msi_intr23),
	IDTVEC(msi_intr24),
	IDTVEC(msi_intr25),
	IDTVEC(msi_intr26),
	IDTVEC(msi_intr27),
	IDTVEC(msi_intr28),
	IDTVEC(msi_intr29),
	IDTVEC(msi_intr30),
	IDTVEC(msi_intr31),
	IDTVEC(msi_intr32),
	IDTVEC(msi_intr33),
	IDTVEC(msi_intr34),
	IDTVEC(msi_intr35),
	IDTVEC(msi_intr36),
	IDTVEC(msi_intr37),
	IDTVEC(msi_intr38),
	IDTVEC(msi_intr39),
	IDTVEC(msi_intr40),
	IDTVEC(msi_intr41),
	IDTVEC(msi_intr42),
	IDTVEC(msi_intr43),
	IDTVEC(msi_intr44),
	IDTVEC(msi_intr45),
	IDTVEC(msi_intr46),
	IDTVEC(msi_intr47),
	IDTVEC(msi_intr48),
	IDTVEC(msi_intr49),
	IDTVEC(msi_intr50),
	IDTVEC(msi_intr51),
	IDTVEC(msi_intr52),
	IDTVEC(msi_intr53),
	IDTVEC(msi_intr54),
	IDTVEC(msi_intr55),
	IDTVEC(msi_intr56),
	IDTVEC(msi_intr57),
	IDTVEC(msi_intr58),
	IDTVEC(msi_intr59),
	IDTVEC(msi_intr60),
	IDTVEC(msi_intr61),
	IDTVEC(msi_intr62),
	IDTVEC(msi_intr63),
	IDTVEC(msi_intr64),
	IDTVEC(msi_intr65),
	IDTVEC(msi_intr66),
	IDTVEC(msi_intr67),
	IDTVEC(msi_intr68),
	IDTVEC(msi_intr69),
	IDTVEC(msi_intr70),
	IDTVEC(msi_intr71),
	IDTVEC(msi_intr72),
	IDTVEC(msi_intr73),
	IDTVEC(msi_intr74),
	IDTVEC(msi_intr75),
	IDTVEC(msi_intr76),
	IDTVEC(msi_intr77),
	IDTVEC(msi_intr78),
	IDTVEC(msi_intr79),
	IDTVEC(msi_intr80),
	IDTVEC(msi_intr81),
	IDTVEC(msi_intr82),
	IDTVEC(msi_intr83),
	IDTVEC(msi_intr84),
	IDTVEC(msi_intr85),
	IDTVEC(msi_intr86),
	IDTVEC(msi_intr87),
	IDTVEC(msi_intr88),
	IDTVEC(msi_intr89),
	IDTVEC(msi_intr90),
	IDTVEC(msi_intr91),
	IDTVEC(msi_intr92),
	IDTVEC(msi_intr93),
	IDTVEC(msi_intr94),
	IDTVEC(msi_intr95),
	IDTVEC(msi_intr96),
	IDTVEC(msi_intr97),
	IDTVEC(msi_intr98),
	IDTVEC(msi_intr99),
	IDTVEC(msi_intr100),
	IDTVEC(msi_intr101),
	IDTVEC(msi_intr102),
	IDTVEC(msi_intr103),
	IDTVEC(msi_intr104),
	IDTVEC(msi_intr105),
	IDTVEC(msi_intr106),
	IDTVEC(msi_intr107),
	IDTVEC(msi_intr108),
	IDTVEC(msi_intr109),
	IDTVEC(msi_intr110),
	IDTVEC(msi_intr111),
	IDTVEC(msi_intr112),
	IDTVEC(msi_intr113),
	IDTVEC(msi_intr114),
	IDTVEC(msi_intr115),
	IDTVEC(msi_intr116),
	IDTVEC(msi_intr117),
	IDTVEC(msi_intr118),
	IDTVEC(msi_intr119),
	IDTVEC(msi_intr120),
	IDTVEC(msi_intr121),
	IDTVEC(msi_intr122),
	IDTVEC(msi_intr123),
	IDTVEC(msi_intr124),
	IDTVEC(msi_intr125),
	IDTVEC(msi_intr126),
	IDTVEC(msi_intr127),
	IDTVEC(msi_intr128),
	IDTVEC(msi_intr129),
	IDTVEC(msi_intr130),
	IDTVEC(msi_intr131),
	IDTVEC(msi_intr132),
	IDTVEC(msi_intr133),
	IDTVEC(msi_intr134),
	IDTVEC(msi_intr135),
	IDTVEC(msi_intr136),
	IDTVEC(msi_intr137),
	IDTVEC(msi_intr138),
	IDTVEC(msi_intr139),
	IDTVEC(msi_intr140),
	IDTVEC(msi_intr141),
	IDTVEC(msi_intr142),
	IDTVEC(msi_intr143),
	IDTVEC(msi_intr144),
	IDTVEC(msi_intr145),
	IDTVEC(msi_intr146),
	IDTVEC(msi_intr147),
	IDTVEC(msi_intr148),
	IDTVEC(msi_intr149),
	IDTVEC(msi_intr150),
	IDTVEC(msi_intr151),
	IDTVEC(msi_intr152),
	IDTVEC(msi_intr153),
	IDTVEC(msi_intr154),
	IDTVEC(msi_intr155),
	IDTVEC(msi_intr156),
	IDTVEC(msi_intr157),
	IDTVEC(msi_intr158),
	IDTVEC(msi_intr159),
	IDTVEC(msi_intr160),
	IDTVEC(msi_intr161),
	IDTVEC(msi_intr162),
	IDTVEC(msi_intr163),
	IDTVEC(msi_intr164),
	IDTVEC(msi_intr165),
	IDTVEC(msi_intr166),
	IDTVEC(msi_intr167),
	IDTVEC(msi_intr168),
	IDTVEC(msi_intr169),
	IDTVEC(msi_intr170),
	IDTVEC(msi_intr171),
	IDTVEC(msi_intr172),
	IDTVEC(msi_intr173),
	IDTVEC(msi_intr174),
	IDTVEC(msi_intr175),
	IDTVEC(msi_intr176),
	IDTVEC(msi_intr177),
	IDTVEC(msi_intr178),
	IDTVEC(msi_intr179),
	IDTVEC(msi_intr180),
	IDTVEC(msi_intr181),
	IDTVEC(msi_intr182),
	IDTVEC(msi_intr183),
	IDTVEC(msi_intr184),
	IDTVEC(msi_intr185),
	IDTVEC(msi_intr186),
	IDTVEC(msi_intr187),
	IDTVEC(msi_intr188),
	IDTVEC(msi_intr189),
	IDTVEC(msi_intr190),
	IDTVEC(msi_intr191);

static inthand_t *msi_intr[IDT_HWI_VECTORS] = {
	&IDTVEC(msi_intr0),
	&IDTVEC(msi_intr1),
	&IDTVEC(msi_intr2),
	&IDTVEC(msi_intr3),
	&IDTVEC(msi_intr4),
	&IDTVEC(msi_intr5),
	&IDTVEC(msi_intr6),
	&IDTVEC(msi_intr7),
	&IDTVEC(msi_intr8),
	&IDTVEC(msi_intr9),
	&IDTVEC(msi_intr10),
	&IDTVEC(msi_intr11),
	&IDTVEC(msi_intr12),
	&IDTVEC(msi_intr13),
	&IDTVEC(msi_intr14),
	&IDTVEC(msi_intr15),
	&IDTVEC(msi_intr16),
	&IDTVEC(msi_intr17),
	&IDTVEC(msi_intr18),
	&IDTVEC(msi_intr19),
	&IDTVEC(msi_intr20),
	&IDTVEC(msi_intr21),
	&IDTVEC(msi_intr22),
	&IDTVEC(msi_intr23),
	&IDTVEC(msi_intr24),
	&IDTVEC(msi_intr25),
	&IDTVEC(msi_intr26),
	&IDTVEC(msi_intr27),
	&IDTVEC(msi_intr28),
	&IDTVEC(msi_intr29),
	&IDTVEC(msi_intr30),
	&IDTVEC(msi_intr31),
	&IDTVEC(msi_intr32),
	&IDTVEC(msi_intr33),
	&IDTVEC(msi_intr34),
	&IDTVEC(msi_intr35),
	&IDTVEC(msi_intr36),
	&IDTVEC(msi_intr37),
	&IDTVEC(msi_intr38),
	&IDTVEC(msi_intr39),
	&IDTVEC(msi_intr40),
	&IDTVEC(msi_intr41),
	&IDTVEC(msi_intr42),
	&IDTVEC(msi_intr43),
	&IDTVEC(msi_intr44),
	&IDTVEC(msi_intr45),
	&IDTVEC(msi_intr46),
	&IDTVEC(msi_intr47),
	&IDTVEC(msi_intr48),
	&IDTVEC(msi_intr49),
	&IDTVEC(msi_intr50),
	&IDTVEC(msi_intr51),
	&IDTVEC(msi_intr52),
	&IDTVEC(msi_intr53),
	&IDTVEC(msi_intr54),
	&IDTVEC(msi_intr55),
	&IDTVEC(msi_intr56),
	&IDTVEC(msi_intr57),
	&IDTVEC(msi_intr58),
	&IDTVEC(msi_intr59),
	&IDTVEC(msi_intr60),
	&IDTVEC(msi_intr61),
	&IDTVEC(msi_intr62),
	&IDTVEC(msi_intr63),
	&IDTVEC(msi_intr64),
	&IDTVEC(msi_intr65),
	&IDTVEC(msi_intr66),
	&IDTVEC(msi_intr67),
	&IDTVEC(msi_intr68),
	&IDTVEC(msi_intr69),
	&IDTVEC(msi_intr70),
	&IDTVEC(msi_intr71),
	&IDTVEC(msi_intr72),
	&IDTVEC(msi_intr73),
	&IDTVEC(msi_intr74),
	&IDTVEC(msi_intr75),
	&IDTVEC(msi_intr76),
	&IDTVEC(msi_intr77),
	&IDTVEC(msi_intr78),
	&IDTVEC(msi_intr79),
	&IDTVEC(msi_intr80),
	&IDTVEC(msi_intr81),
	&IDTVEC(msi_intr82),
	&IDTVEC(msi_intr83),
	&IDTVEC(msi_intr84),
	&IDTVEC(msi_intr85),
	&IDTVEC(msi_intr86),
	&IDTVEC(msi_intr87),
	&IDTVEC(msi_intr88),
	&IDTVEC(msi_intr89),
	&IDTVEC(msi_intr90),
	&IDTVEC(msi_intr91),
	&IDTVEC(msi_intr92),
	&IDTVEC(msi_intr93),
	&IDTVEC(msi_intr94),
	&IDTVEC(msi_intr95),
	&IDTVEC(msi_intr96),
	&IDTVEC(msi_intr97),
	&IDTVEC(msi_intr98),
	&IDTVEC(msi_intr99),
	&IDTVEC(msi_intr100),
	&IDTVEC(msi_intr101),
	&IDTVEC(msi_intr102),
	&IDTVEC(msi_intr103),
	&IDTVEC(msi_intr104),
	&IDTVEC(msi_intr105),
	&IDTVEC(msi_intr106),
	&IDTVEC(msi_intr107),
	&IDTVEC(msi_intr108),
	&IDTVEC(msi_intr109),
	&IDTVEC(msi_intr110),
	&IDTVEC(msi_intr111),
	&IDTVEC(msi_intr112),
	&IDTVEC(msi_intr113),
	&IDTVEC(msi_intr114),
	&IDTVEC(msi_intr115),
	&IDTVEC(msi_intr116),
	&IDTVEC(msi_intr117),
	&IDTVEC(msi_intr118),
	&IDTVEC(msi_intr119),
	&IDTVEC(msi_intr120),
	&IDTVEC(msi_intr121),
	&IDTVEC(msi_intr122),
	&IDTVEC(msi_intr123),
	&IDTVEC(msi_intr124),
	&IDTVEC(msi_intr125),
	&IDTVEC(msi_intr126),
	&IDTVEC(msi_intr127),
	&IDTVEC(msi_intr128),
	&IDTVEC(msi_intr129),
	&IDTVEC(msi_intr130),
	&IDTVEC(msi_intr131),
	&IDTVEC(msi_intr132),
	&IDTVEC(msi_intr133),
	&IDTVEC(msi_intr134),
	&IDTVEC(msi_intr135),
	&IDTVEC(msi_intr136),
	&IDTVEC(msi_intr137),
	&IDTVEC(msi_intr138),
	&IDTVEC(msi_intr139),
	&IDTVEC(msi_intr140),
	&IDTVEC(msi_intr141),
	&IDTVEC(msi_intr142),
	&IDTVEC(msi_intr143),
	&IDTVEC(msi_intr144),
	&IDTVEC(msi_intr145),
	&IDTVEC(msi_intr146),
	&IDTVEC(msi_intr147),
	&IDTVEC(msi_intr148),
	&IDTVEC(msi_intr149),
	&IDTVEC(msi_intr150),
	&IDTVEC(msi_intr151),
	&IDTVEC(msi_intr152),
	&IDTVEC(msi_intr153),
	&IDTVEC(msi_intr154),
	&IDTVEC(msi_intr155),
	&IDTVEC(msi_intr156),
	&IDTVEC(msi_intr157),
	&IDTVEC(msi_intr158),
	&IDTVEC(msi_intr159),
	&IDTVEC(msi_intr160),
	&IDTVEC(msi_intr161),
	&IDTVEC(msi_intr162),
	&IDTVEC(msi_intr163),
	&IDTVEC(msi_intr164),
	&IDTVEC(msi_intr165),
	&IDTVEC(msi_intr166),
	&IDTVEC(msi_intr167),
	&IDTVEC(msi_intr168),
	&IDTVEC(msi_intr169),
	&IDTVEC(msi_intr170),
	&IDTVEC(msi_intr171),
	&IDTVEC(msi_intr172),
	&IDTVEC(msi_intr173),
	&IDTVEC(msi_intr174),
	&IDTVEC(msi_intr175),
	&IDTVEC(msi_intr176),
	&IDTVEC(msi_intr177),
	&IDTVEC(msi_intr178),
	&IDTVEC(msi_intr179),
	&IDTVEC(msi_intr180),
	&IDTVEC(msi_intr181),
	&IDTVEC(msi_intr182),
	&IDTVEC(msi_intr183),
	&IDTVEC(msi_intr184),
	&IDTVEC(msi_intr185),
	&IDTVEC(msi_intr186),
	&IDTVEC(msi_intr187),
	&IDTVEC(msi_intr188),
	&IDTVEC(msi_intr189),
	&IDTVEC(msi_intr190),
	&IDTVEC(msi_intr191)
};

void
msi_setup(int intr, int cpuid)
{
	setidt(IDT_OFFSET + intr, msi_intr[intr],
	    SDT_SYSIGT, SEL_KPL, 0, cpuid);
}

void
msi_map(int intr, uint64_t *addr, uint32_t *data, int cpuid)
{
	int vector, lapic_id;

	vector = IDT_OFFSET + intr;
	lapic_id = CPUID_TO_APICID(cpuid);

	*addr = MSI_X86_ADDR(lapic_id);
	*data = MSI_X86_DATA(vector);
}
