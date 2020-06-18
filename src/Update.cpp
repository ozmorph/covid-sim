#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "Update.h"
#include "Model.h"
#include "ModelMacros.h"
#include "Param.h"
#include "InfStat.h"
#include "Bitmap.h"
#include "Rand.h"

//adding function to record an event: ggilani - 10/10/2014
void RecordEvent(double, int, int, int, int); //added int as argument to InfectSweep to record run number: ggilani - 15/10/14

unsigned short int ChooseFromICDF(double *, double, int);
Severity ChooseFinalDiseaseSeverity(int, int);

// state transition helpers
void SusceptibleToRecovered(int cellIndex);
void SusceptibleToLatent(int cellIndex);
void LatentToInfectious(int cellIndex);
void InfectiousToRecovered(int cellIndex);
void InfectiousToDeath(int cellIndex);

void DoImmune(int ai)
{
	// This transfers a person straight from susceptible to immune. Used to start a run with a partially immune population.
	int x, y;

	auto& person = *(Hosts + ai);
	if (person.inf == InfStat_Susceptible)
	{
		person.inf = InfStat_ImmuneAtStart;

		SusceptibleToRecovered(person.pcell);
		auto& cell = Cells[person.pcell];

		if (person.listpos < cell.S)
		{
			cell.susceptible[person.listpos] = cell.susceptible[cell.S];
			Hosts[cell.susceptible[person.listpos]].listpos = person.listpos;
		}
		if (cell.L > 0)
		{
			cell.susceptible[cell.S] = cell.susceptible[cell.S + cell.L];
			Hosts[cell.susceptible[cell.S]].listpos = cell.S;
		}
		if (cell.I > 0)
		{
			cell.susceptible[cell.S + cell.L] = cell.susceptible[cell.S + cell.L + cell.I];
			Hosts[cell.susceptible[cell.S + cell.L]].listpos = cell.S + cell.L;
		}
		if (person.listpos < cell.S + cell.L + cell.I)
		{
			cell.susceptible[cell.S + cell.L + cell.I] = ai;
			person.listpos = cell.S + cell.L + cell.I;
		}

		if (P.OutputBitmap)
		{
			x = ((int)(Households[person.hh].loc.x * P.scale.x)) - P.bmin.x;
			y = ((int)(Households[person.hh].loc.y * P.scale.y)) - P.bmin.y;
			if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
			{
				unsigned j = y * bmh->width + x;
				if (j < bmh->imagesize)
				{
#pragma omp atomic
					bmRecovered[j]++;
				}
			}
		}
	}
}
void DoInfect(int ai, double t, int tn, int run) // Change person from susceptible to latently infected.  added int as argument to DoInfect to record run number: ggilani - 15/10/14
{
	///// This updates a number of things concerning person ai (and their contacts/infectors/places etc.) at time t in thread tn for this run.
	int i;
	unsigned short int ts; //// time step
	double q, x, y; //// q radius squared, x and y coords. q later changed to be quantile of inverse CDF to choose latent period.

	auto& person = *(Hosts + ai); //// pointer arithmetic. a = pointer to person. ai = int person index.
	const auto age_group = HOST_AGE_GROUP(ai);

	if (person.inf == InfStat_Susceptible) //// Only change anything if person a/ai uninfected at start of this function.
	{
		ts = (unsigned short int) (P.TimeStepsPerDay * t);
		person.inf = InfStat_Latent; //// set person a to be infected
		person.infection_time = (unsigned short int) ts; //// record their infection time
		///// Change threaded state variables to reflect new infection status of person a.
		auto& state = StateT[tn];
		state.cumI++;
		state.cumItype[person.infect_type % INFECT_TYPE_MASK]++;
		state.cumIa[age_group]++;
		//// calculate radius squared, and increment sum of radii squared.
		x = (Households[person.hh].loc.x - P.LocationInitialInfection[0][0]);
		y = (Households[person.hh].loc.y - P.LocationInitialInfection[0][1]);
		q = x * x + y * y;
		state.sumRad2 += q;

		if (q > state.maxRad2) state.maxRad2 = q; //// update maximum radius squared from seeding infection
		{
			SusceptibleToLatent(person.pcell);
			auto& cell = Cells[person.pcell];

			if (person.listpos < cell.S)
			{
				cell.susceptible[person.listpos] = cell.susceptible[cell.S];
				Hosts[cell.susceptible[person.listpos]].listpos = person.listpos;
				person.listpos = cell.S;	//// person a's position with cell.members now equal to number of susceptibles in cell.
				cell.latent[0] = ai; //// person ai joins front of latent queue.
			}
		}
		state.cumI_keyworker[person.keyworker]++;

		if (P.DoLatent)
		{
			i = (int)floor((q = ranf_mt(tn) * CDF_RES));
			q -= ((double)i);
			person.latent_time = (unsigned short int) floor(0.5 + (t - P.LatentPeriod * log(q * P.latent_icdf[i + 1] + (1.0 - q) * P.latent_icdf[i])) * P.TimeStepsPerDay);
		}
		else
			person.latent_time = (unsigned short int) (t * P.TimeStepsPerDay);
		if (person.infector >= 0) // record generation times and serial intervals
		{
			state.cumTG += (((int)person.infection_time) - ((int)Hosts[person.infector].infection_time));
			state.cumSI += (((int)person.latent_time) - ((int)Hosts[person.infector].latent_time));
			state.nTG++;
		}

		//if (P.DoLatent)	a->latent_time = a->infection_time + ChooseFromICDF(P.latent_icdf, P.LatentPeriod, tn);
		//else			a->latent_time = (unsigned short int) (t * P.TimeStepsPerDay);

		if (P.DoAdUnits)
		{
			auto const& adunit = Mcells[person.mcell].adunit;
			state.cumI_adunit[adunit]++;

			if (P.OutputAdUnitAge)
			{
				state.prevInf_age_adunit[age_group][adunit]++;
				state.cumInf_age_adunit [age_group][adunit]++;
			}
		}
		if (P.OutputBitmap)
		{
			if ((P.OutputBitmapDetected == 0) || ((P.OutputBitmapDetected == 1) && (Hosts[ai].detected == 1)))
			{
				int ix = ((int)(Households[person.hh].loc.x * P.scale.x)) - P.bmin.x;
				int iy = ((int)(Households[person.hh].loc.y * P.scale.y)) - P.bmin.y;
				if ((ix >= 0) && (ix < P.b.width) && (iy >= 0) && (iy < P.b.height))
				{
					unsigned j = iy * bmh->width + ix;
					if (j < bmh->imagesize)
					{
#pragma omp atomic
						bmInfected[j]++;
					}
				}
			}
		}
		//added this to record event if flag is set to 1 : ggilani - 10/10/2014
		if (P.DoRecordInfEvents)
		{
			RecordEvent(t, ai, run, 0, tn); //added int as argument to RecordEvent to record run number: ggilani - 15/10/14
		}
		if ((t > 0) && (P.DoOneGen))
		{
			DoIncub(ai, ts, tn, run);
			DoCase(ai, t, ts, tn);
			DoRecover(ai, tn, run);
		}
	}
}

void RecordEvent(double t, int ai, int run, int type, int tn) //added int as argument to RecordEvent to record run number: ggilani - 15/10/14
{
	/* Function: RecordEvent(t, ai)
	 * Records an infection event in the event log
	 *
	 * Parameters:
	 *	t: time of infection event
	 *	ai: index of infectee
	 *
	 * Returns: void
	 *
	 * Author: ggilani, Date: 10/10/2014
	 */
	 //Declare int to store infector's index
	int bi;

	bi = Hosts[ai].infector;

	//Save information to event
#pragma omp critical (inf_event)
	if (nEvents < P.MaxInfEvents)
	{
		InfEventLog[nEvents].run = run;
		InfEventLog[nEvents].type = type;
		InfEventLog[nEvents].t = t;
		InfEventLog[nEvents].infectee_ind = ai;
		InfEventLog[nEvents].infectee_adunit = Mcells[Hosts[ai].mcell].adunit;
		InfEventLog[nEvents].infectee_x = Households[Hosts[ai].hh].loc.x + P.SpatialBoundingBox[0];
		InfEventLog[nEvents].infectee_y = Households[Hosts[ai].hh].loc.y + P.SpatialBoundingBox[1];
		InfEventLog[nEvents].listpos = Hosts[ai].listpos;
		InfEventLog[nEvents].infectee_cell = Hosts[ai].pcell;
		InfEventLog[nEvents].thread = tn;
		if (type == 0) //infection event - record time of onset of infector and infector
		{
			InfEventLog[nEvents].infector_ind = bi;
			if (bi < 0)
			{
				InfEventLog[nEvents].t_infector = -1;
				InfEventLog[nEvents].infector_cell = -1;
			}
			else
			{
				InfEventLog[nEvents].t_infector = (int)(Hosts[bi].infection_time / P.TimeStepsPerDay);
				InfEventLog[nEvents].infector_cell = Hosts[bi].pcell;
			}
		}
		else if (type == 1) //onset event - record infectee's onset time
		{
			InfEventLog[nEvents].t_infector = (int)(Hosts[ai].infection_time / P.TimeStepsPerDay);
		}
		else if ((type == 2) || (type == 3)) //recovery or death event - record infectee's onset time
		{
			InfEventLog[nEvents].t_infector = (int)(Hosts[ai].latent_time / P.TimeStepsPerDay);
		}

		//increment the index of the infection event
		nEvents++;
	}

}

void DoMild(int ai, int tn)
{
	if (P.DoSeverity) //// shouldn't need this but best be careful.
	{
		auto& person = *(Hosts + ai);
		if (person.Severity_Current == Severity_Asymptomatic)
		{
			person.Severity_Current = Severity_Mild;
			auto& state = StateT[tn];
			state.Mild++;
			state.cumMild++;
			state.Mild_age[HOST_AGE_GROUP(ai)]++;
			state.cumMild_age[HOST_AGE_GROUP(ai)]++;
			if (P.DoAdUnits)
			{
				const auto adunit = Mcells[person.mcell].adunit;
				state.Mild_adunit	[adunit]++;
				state.cumMild_adunit[adunit]++;
			}
		}
	}
}
void DoILI(int ai, int tn)
{
	if (P.DoSeverity) //// shouldn't need this but best be careful.
	{
		auto& person = *(Hosts + ai);
		if (person.Severity_Current == Severity_Asymptomatic)
		{
			person.Severity_Current = Severity_ILI;

			auto& state = StateT[tn];
			const auto age_group = HOST_AGE_GROUP(ai);
			state.ILI++;
			state.cumILI++;
			state.ILI_age[age_group]++;
			state.cumILI_age[age_group]++;
			if (P.DoAdUnits)
			{
				const auto adunit = Mcells[person.mcell].adunit;
				state.ILI_adunit	[adunit]++;
				state.cumILI_adunit	[adunit]++;
			}
		}
	}
}
void DoSARI(int ai, int tn)
{
	if (P.DoSeverity) //// shouldn't need this but best be careful.
	{
		auto& person = *(Hosts + ai);
		if (person.Severity_Current == Severity_ILI)
		{
			person.Severity_Current = Severity_SARI;

			auto& state = StateT[tn];
			const auto age_group = HOST_AGE_GROUP(ai);
			state.ILI--;
			state.ILI_age[age_group]--;
			state.SARI++;
			state.cumSARI++;
			state.SARI_age[age_group]++;
			state.cumSARI_age[age_group]++;
			if (P.DoAdUnits)
			{
				const auto adunit = Mcells[person.mcell].adunit;
				state.ILI_adunit		[adunit]--;
				state.SARI_adunit		[adunit]++;
				state.cumSARI_adunit	[adunit]++;
			}
		}
	}
}
void DoCritical(int ai, int tn)
{
	if (P.DoSeverity) //// shouldn't need this but best be careful.
	{
		auto& person = *(Hosts + ai);
		if (person.Severity_Current == Severity_SARI)
		{
			person.Severity_Current = Severity_Critical;

			auto& state = StateT[tn];
			const auto age_group = HOST_AGE_GROUP(ai);
			state.SARI--;
			state.SARI_age[age_group]--;
			state.Critical++;
			state.cumCritical++;
			state.Critical_age[age_group]++;
			state.cumCritical_age[age_group]++;
			if (P.DoAdUnits)
			{
				const auto adunit = Mcells[person.mcell].adunit;
				state.SARI_adunit			[adunit]--;
				state.Critical_adunit		[adunit]++;
				state.cumCritical_adunit	[adunit]++;
			}
		}
	}
}
void DoRecoveringFromCritical(int ai, int tn)
{
	//// note function different from DoRecover_FromSeverity.
	//// DoRecover_FromSeverity assigns people to state Recovered (and bookkeeps accordingly).
	//// DoRecoveringFromCritical assigns people to intermediate state "recovering from critical condition" (and bookkeeps accordingly).
	if (P.DoSeverity) //// shouldn't need this but best be careful.
	{
		auto& person = *(Hosts + ai);
		if (person.Severity_Current == Severity_Critical && (!person.to_die)) //// second condition should be unnecessary but leave in for now.
		{
			person.Severity_Current = Severity_RecoveringFromCritical;

			auto& state = StateT[tn];
			const auto age_group = HOST_AGE_GROUP(ai);
			state.Critical--;
			state.Critical_age[age_group]--;
			state.CritRecov++;
			state.cumCritRecov++;
			state.CritRecov_age[age_group]++;
			state.cumCritRecov_age[age_group]++;
			if (P.DoAdUnits)
			{
				const auto adunit = Mcells[person.mcell].adunit;
				state.Critical_adunit[adunit]--;
				state.CritRecov_adunit[adunit]++;
				state.cumCritRecov_adunit[adunit]++;
			}
		}
	}
}
void DoDeath_FromCriticalorSARIorILI(int ai, int tn)
{
	auto& person = *(Hosts + ai);
	if (P.DoSeverity)
	{
		const auto adunit = Mcells[person.mcell].adunit;
		const auto age_group = HOST_AGE_GROUP(ai);
		auto& state = StateT[tn];

		if (person.Severity_Current == Severity_Critical)
		{
			state.Critical--;
			state.Critical_age[age_group]--;
			state.cumDeath_Critical++;
			state.cumDeath_Critical_age[age_group]++;
			if (P.DoAdUnits)
			{
				state.Critical_adunit			[adunit]--;
				state.cumDeath_Critical_adunit	[adunit]++;
			}
			//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
			person.Severity_Current = Severity_Dead;
		}
		else if (person.Severity_Current == Severity_SARI)
		{
			state.SARI--;
			state.SARI_age[age_group]--;
			state.cumDeath_SARI++;
			state.cumDeath_SARI_age[age_group]++;
			if (P.DoAdUnits)
			{
				state.SARI_adunit			[adunit]--;
				state.cumDeath_SARI_adunit	[adunit]++;
			}
			//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
			person.Severity_Current = Severity_Dead;
		}
		else if (person.Severity_Current == Severity_ILI)
		{
			state.ILI--;
			state.ILI_age[age_group]--;
			state.cumDeath_ILI++;
			state.cumDeath_ILI_age[age_group]++;
			if (P.DoAdUnits)
			{
				state.ILI_adunit			[adunit]--;
				state.cumDeath_ILI_adunit	[adunit]++;
			}
			//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
			person.Severity_Current = Severity_Dead;
		}
	}
}
void DoRecover_FromSeverity(int ai, int tn)
{
	//// note function different from DoRecoveringFromCritical.
	//// DoRecover_FromSeverity assigns people to state Recovered (and bookkeeps accordingly).
	//// DoRecoveringFromCritical assigns people to intermediate state "recovering from critical condition" (and bookkeeps accordingly).

	//// moved this from DoRecover
	auto& person = *(Hosts + ai);

	if (P.DoSeverity)
		if (person.inf == InfStat_InfectiousAsymptomaticNotCase || person.inf == InfStat_Case) ///// i.e same condition in DoRecover (make sure you don't recover people twice).
		{
			const auto adunit = Mcells[person.mcell].adunit;
			const auto age_group = HOST_AGE_GROUP(ai);
			auto& state = StateT[tn];

			if (person.Severity_Current == Severity_Mild)
			{
				state.Mild--;
				state.Mild_age[age_group]--;
				if (P.DoAdUnits) state.Mild_adunit[adunit]--;
				//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
				person.Severity_Current = Severity_Recovered;
			}
			else if (person.Severity_Current == Severity_ILI)
			{
				state.ILI--;
				state.ILI_age[age_group]--;
				if (P.DoAdUnits) state.ILI_adunit[adunit]--;
				//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
				person.Severity_Current = Severity_Recovered;
			}
			else if (person.Severity_Current == Severity_SARI)
			{
				state.SARI--;
				state.SARI_age[age_group]--;
				if (P.DoAdUnits) state.SARI_adunit[adunit]--;
				//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
				person.Severity_Current = Severity_Recovered;
			}
			else if (person.Severity_Current == Severity_RecoveringFromCritical)
			{
				state.CritRecov--; //// decrement CritRecov, not critical.
				state.CritRecov_age[age_group]--;
				if (P.DoAdUnits) state.CritRecov_adunit[adunit]--;
				//// change current status (so that flags work if function called again for same person). Don't move this outside of this if statement, even though it looks like it can be moved safely. It can't.
				person.Severity_Current = Severity_Recovered;
			}
		}
}

void DoIncub(int ai, unsigned short int ts, int tn, int run)
{
	double q;

	int age = HOST_AGE_GROUP(ai);
	if (age >= NUM_AGE_GROUPS) age = NUM_AGE_GROUPS - 1;

	auto& person = *(Hosts + ai);
	if (person.inf == InfStat_Latent)
	{
		person.infectiousness = (float)P.AgeInfectiousness[age];
		if (P.InfectiousnessSD > 0) person.infectiousness *= (float) gen_gamma_mt(1 / (P.InfectiousnessSD * P.InfectiousnessSD), 1 / (P.InfectiousnessSD * P.InfectiousnessSD), tn);
		q = P.ProportionSymptomatic[age]
			* (HOST_TREATED(ai) ? (1 - P.TreatSympDrop) : 1)
			* (HOST_VACCED(ai) ? (1 - P.VaccSympDrop) : 1);

		if (ranf_mt(tn) < q)
		{
			person.inf = InfStat_InfectiousAlmostSymptomatic;
			person.infectiousness *= (float)(-P.SymptInfectiousness);
		}
		else
			person.inf = InfStat_InfectiousAsymptomaticNotCase;

		if (!P.DoSeverity || person.inf == InfStat_InfectiousAsymptomaticNotCase) //// if not doing severity or if person asymptomatic.
		{
			if (P.DoInfectiousnessProfile)	person.recovery_or_death_time = person.latent_time + (unsigned short int) (P.InfectiousPeriod * P.TimeStepsPerDay);
			else							person.recovery_or_death_time = person.latent_time + ChooseFromICDF(P.infectious_icdf, P.InfectiousPeriod, tn);
		}
		else
		{
			int CaseTime = person.latent_time + ((int)(P.LatentToSymptDelay / P.TimeStep)); //// base severity times on CaseTime, not latent time. Otherwise there are edge cases where recovery time is zero days after latent_time and therefore before DoCase called in IncubRecoverySweep (i.e. people can recover before they've become a case!).

			//// choose final disease severity (either mild, ILI, SARI, Critical, not asymptomatic as covered above) by age
			person.Severity_Final = ChooseFinalDiseaseSeverity(age, tn);

			/// choose outcome recovery or death
			if (	((person.Severity_Final == Severity_Critical)	&& (ranf_mt(tn) < P.CFR_Critical_ByAge	[age]))		||
					((person.Severity_Final == Severity_SARI	)	&& (ranf_mt(tn) < P.CFR_SARI_ByAge		[age]))		||
					((person.Severity_Final == Severity_ILI		)	&& (ranf_mt(tn) < P.CFR_ILI_ByAge		[age]))		)
				person.to_die = 1;

			//// choose events and event times
			if (person.Severity_Final == Severity_Mild)
				person.recovery_or_death_time = CaseTime 	+ ChooseFromICDF(P.MildToRecovery_icdf	, P.Mean_MildToRecovery[age], tn);
			else if (person.Severity_Final == Severity_Critical)
			{
				person.SARI_time 		= CaseTime			+ ChooseFromICDF(P.ILIToSARI_icdf		, P.Mean_ILIToSARI[age], tn);
				person.Critical_time 	= person.SARI_time	+ ChooseFromICDF(P.SARIToCritical_icdf	, P.Mean_SARIToCritical[age], tn);
				if (person.to_die)
					person.recovery_or_death_time		= person.Critical_time					+ ChooseFromICDF(P.CriticalToDeath_icdf		, P.Mean_CriticalToDeath[age], tn);
				else
				{
					person.RecoveringFromCritical_time	= person.Critical_time					+ ChooseFromICDF(P.CriticalToCritRecov_icdf	, P.Mean_CriticalToCritRecov[age], tn);
					person.recovery_or_death_time		= person.RecoveringFromCritical_time	+ ChooseFromICDF(P.CritRecovToRecov_icdf	, P.Mean_CritRecovToRecov[age], tn);
				}
			}
			else if (person.Severity_Final == Severity_SARI)
			{
				person.SARI_time = CaseTime + ChooseFromICDF(P.ILIToSARI_icdf, P.Mean_ILIToSARI[age], tn);
				if (person.to_die)
					person.recovery_or_death_time = person.SARI_time + ChooseFromICDF(P.SARIToDeath_icdf	, P.Mean_SARIToDeath[age], tn);
				else
					person.recovery_or_death_time = person.SARI_time + ChooseFromICDF(P.SARIToRecovery_icdf	, P.Mean_SARIToRecovery[age], tn);
			}
			else /*i.e. if Severity_Final == Severity_ILI*/
			{
				if (person.to_die)
					person.recovery_or_death_time = CaseTime + ChooseFromICDF(P.ILIToDeath_icdf		, P.Mean_ILIToDeath[age], tn);
				else
					person.recovery_or_death_time = CaseTime + ChooseFromICDF(P.ILIToRecovery_icdf	, P.Mean_ILIToRecovery[age], tn);
			}
		}

		if ((person.inf== InfStat_InfectiousAlmostSymptomatic) && ((P.ControlPropCasesId == 1) || (ranf_mt(tn) < P.ControlPropCasesId)))
		{
			Hosts[ai].detected = 1;
			Hosts[ai].detected_time = ts + (unsigned short int)(P.LatentToSymptDelay * P.TimeStepsPerDay);


			if ((P.DoDigitalContactTracing) && (Hosts[ai].detected_time >= (unsigned short int)(AdUnits[Mcells[Hosts[ai].mcell].adunit].DigitalContactTracingTimeStart * P.TimeStepsPerDay)) && (Hosts[ai].detected_time < (unsigned short int)((AdUnits[Mcells[Hosts[ai].mcell].adunit].DigitalContactTracingTimeStart + P.DigitalContactTracingPolicyDuration)*P.TimeStepsPerDay)) && (Hosts[ai].digitalContactTracingUser))
			{
				//set dct_trigger_time for index case
			if (P.DoDigitalContactTracing)	//set dct_trigger_time for index case
				if (Hosts[ai].dct_trigger_time == (USHRT_MAX - 1)) //if this hasn't been set in DigitalContactTracingSweep due to detection of contact of contacts, set it here
					Hosts[ai].dct_trigger_time = Hosts[ai].detected_time + (unsigned short int) (P.DelayFromIndexCaseDetectionToDCTIsolation * P.TimeStepsPerDay);
			}
		}

		//// update pointers
		LatentToInfectious(person.pcell);
		auto& cell = Cells[person.pcell];
		
		if (cell.L > 0)
		{
			cell.susceptible[person.listpos] = cell.latent[cell.L]; //// reset pointers.
			Hosts[cell.susceptible[person.listpos]].listpos = person.listpos;
			person.listpos = cell.S + cell.L; //// change person a's listpos, which will now refer to their position among infectious people, not latent.
			cell.infected[0] = ai; //// this person is now first infectious person in the array. Pointer was moved back one so now that memory address refers to person ai. Alternative would be to move everyone back one which would take longer.
		}
	}
}

void DoDetectedCase(int ai, double t, unsigned short int ts, int tn)
{
	//// Function DoDetectedCase does many things associated with various interventions.
	//// Enacts Household quarantine, case isolation, place closure.
	//// and therefore changes lots of quantities (e.g. quar_comply and isolation_start_time) associated with model macros e.g. HOST_ABSENT / HOST_ISOLATED

	int j, k, f, j1, j2, ad; // m, h, ad;
	auto& person = *(Hosts + ai);
	auto& cell = Mcells[person.mcell];
	auto const& adunit = AdUnits[cell.adunit];

	//// Increment triggers (Based on numbers of detected cases) for interventions. Used in TreatSweep function when not doing Global or Admin triggers. And not when doing ICU triggers.
	if (cell.treat_trig				< USHRT_MAX - 1) cell.treat_trig++;
	if (cell.vacc_trig				< USHRT_MAX - 1) cell.vacc_trig++;
	if (cell.move_trig				< USHRT_MAX - 1) cell.move_trig++;
	if (cell.socdist_trig			< USHRT_MAX - 1) cell.socdist_trig++;
	if (cell.keyworkerproph_trig	< USHRT_MAX - 1) cell.keyworkerproph_trig++;

	if (!P.AbsenteeismPlaceClosure)
	{
		if ((P.PlaceCloseRoundHousehold)&& (cell.place_trig < USHRT_MAX - 1)) cell.place_trig++;
		if ((t >= P.PlaceCloseTimeStart) && (!P.DoAdminTriggers) && (!((P.DoGlobalTriggers)&&(P.PlaceCloseCellIncThresh<1000000000))))
			for (j = 0; j < P.PlaceTypeNum; j++)
				if ((j != P.HotelPlaceType) && (person.PlaceLinks[j] >= 0))
				{
					DoPlaceClose(j, person.PlaceLinks[j], ts, tn, 0);
					if (!P.PlaceCloseRoundHousehold)
					{
						if (Mcells[Places[j][person.PlaceLinks[j]].mcell].place_trig < USHRT_MAX - 1)
						{
#pragma omp critical (place_trig)
							Mcells[Places[j][person.PlaceLinks[j]].mcell].place_trig++;
						}
					}
				}
	}

	if (t >= P.TreatTimeStart)
		if ((P.TreatPropCases == 1) || (ranf_mt(tn) < P.TreatPropCases))
		{
			DoTreatCase(ai, ts, tn);
			if (P.DoHouseholds)
			{
				if ((t < P.TreatTimeStart + P.TreatHouseholdsDuration) && ((P.TreatPropCaseHouseholds == 1) || (ranf_mt(tn) < P.TreatPropCaseHouseholds)))
				{
					j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
					for (j = j1; j < j2; j++)
						if (!HOST_TO_BE_TREATED(j)) DoProph(j, ts, tn);
				}
			}
			if (P.DoPlaces)
			{
				if (t < P.TreatTimeStart + P.TreatPlaceGeogDuration)
					for (j = 0; j < P.PlaceTypeNum; j++)
						if (person.PlaceLinks[j] >= 0)
						{
							if (P.DoPlaceGroupTreat)
							{
								if ((P.TreatPlaceProbCaseId[j] == 1) || (ranf_mt(tn) < P.TreatPlaceProbCaseId[j]))
								{
									StateT[tn].p_queue[j][StateT[tn].np_queue[j]] = person.PlaceLinks[j];
									StateT[tn].pg_queue[j][StateT[tn].np_queue[j]++] = person.PlaceGroupLinks[j];
								}
							}
							else
							{
								f = 0;
#pragma omp critical (starttreat)
								if (!Places[j][person.PlaceLinks[j]].treat) f = Places[j][person.PlaceLinks[j]].treat = 1;
								if (f)
								{
									if ((P.TreatPlaceProbCaseId[j] == 1) || (ranf_mt(tn) < P.TreatPlaceProbCaseId[j]))
										StateT[tn].p_queue[j][StateT[tn].np_queue[j]++] = person.PlaceLinks[j];
									else
										Places[j][person.PlaceLinks[j]].treat = 0;
								}
							}
						}
			}
		}
	if (P.DoHouseholds)
	{
		if ((!P.DoMassVacc) && (t >= P.VaccTimeStart))
		{
			// DoVacc is going to test that `State.cumV < P.VaccMaxCourses` itself before
			// incrementing State.cumV, but by checking here too we can avoid a lot of
			// wasted effort
			bool cumV_OK;
#pragma omp critical (state_cumV)
			{
				cumV_OK = State.cumV < P.VaccMaxCourses;
			}
			if (cumV_OK && (t < P.VaccTimeStart + P.VaccHouseholdsDuration) && ((P.VaccPropCaseHouseholds == 1) || (ranf_mt(tn) < P.VaccPropCaseHouseholds)))
			{
				j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
				for (j = j1; j < j2; j++) DoVacc(j, ts);
			}
		}

		//// Giant compound if statement. If doing delays by admin unit, then window of HQuarantine dependent on admin unit-specific duration. This if statement ensures that this timepoint within window, regardless of how window defined.
		if ((P.DoInterventionDelaysByAdUnit &&
			(t >= adunit.HQuarantineTimeStart		&&	(t < adunit.HQuarantineTimeStart + adunit.HQuarantineDuration)))		||
			(t >= adunit.HQuarantineTimeStart		&&	(t < adunit.HQuarantineTimeStart + P.HQuarantinePolicyDuration))									)
		{
			j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
			if ((!HOST_TO_BE_QUARANTINED(j1)) || (P.DoHQretrigger))
			{
				Hosts[j1].quar_start_time = ts + ((unsigned short int) (P.TimeStepsPerDay * P.HQuarantineDelay));
				k = (ranf_mt(tn) < P.HQuarantinePropHouseCompliant) ? 1 : 0; //// Is household compliant? True or false
				if (k) StateT[tn].cumHQ++; ////  if compliant, increment cumulative numbers of households under quarantine.
				//// if household not compliant then neither is first person. Otheswise ask whether first person is compliant?
				///// cycle through remaining household members and repeat the above steps
				for (j = j1; j < j2; j++)
				{
					if(j>j1) Hosts[j].quar_start_time = Hosts[j1].quar_start_time;
					Hosts[j].quar_comply = ((k == 0) ? 0 : ((ranf_mt(tn) < P.HQuarantinePropIndivCompliant) ? 1 : 0));
					if ((Hosts[j].quar_comply) && (!HOST_ABSENT(j)))
					{
						if (HOST_AGE_YEAR(j) >= P.CaseAbsentChildAgeCutoff)
						{
							if (Hosts[j].PlaceLinks[P.PlaceTypeNoAirNum - 1] >= 0) StateT[tn].cumAH++;
						}
						else	StateT[tn].cumACS++;
					}
				}
			}
		}
	}

	//// Giant compound if statement. If doing delays by admin unit, then window of case isolation dependent on admin unit-specific duration. This if statement ensures that this timepoint within window, regardless of how window defined.
	if ((P.DoInterventionDelaysByAdUnit &&
		(t >= adunit.CaseIsolationTimeStart && (t < adunit.CaseIsolationTimeStart + adunit.CaseIsolationPolicyDuration)))	||
		(t >= adunit.CaseIsolationTimeStart && (t < adunit.CaseIsolationTimeStart + P.CaseIsolationPolicyDuration))								)
		if ((P.CaseIsolationProp == 1) || (ranf_mt(tn) < P.CaseIsolationProp))
		{
			Hosts[ai].isolation_start_time = ts; //// set isolation start time.
			if (HOST_ABSENT(ai))
			{
				if (person.absent_stop_time < ts + P.usCaseAbsenteeismDelay + P.usCaseIsolationDuration) //// ensure that absent_stop_time is at least now + CaseIsolationDuraton
					person.absent_stop_time = ts + P.usCaseAbsenteeismDelay + P.usCaseIsolationDuration;
			}
			else if (P.DoRealSymptWithdrawal) /* This calculates adult absenteeism from work due to care of isolated children.  */
			{
				Hosts[ai].absent_start_time = ts + P.usCaseIsolationDelay;
				Hosts[ai].absent_stop_time	= ts + P.usCaseIsolationDelay + P.usCaseIsolationDuration;
				if (P.DoPlaces)
				{
					if ((!HOST_QUARANTINED(ai)) && (Hosts[ai].PlaceLinks[P.PlaceTypeNoAirNum - 1] >= 0) && (HOST_AGE_YEAR(ai) >= P.CaseAbsentChildAgeCutoff))
						StateT[tn].cumAC++;
				}
				if ((P.DoHouseholds) && (P.DoPlaces) && (HOST_AGE_YEAR(ai) < P.CaseAbsentChildAgeCutoff)) //// if host is a child who requires adult to stay at home.
				{
					if (!HOST_QUARANTINED(ai)) StateT[tn].cumACS++;
					if (Hosts[ai].ProbCare < P.CaseAbsentChildPropAdultCarers) //// if adult needs to stay at home (i.e. if Proportion of children at home for whom one adult also stays at home = 1 or coinflip satisfied.)
					{
						j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
						f = 0;

						//// in loop below, f true if any household member a) alive AND b) not a child AND c) has no links to workplace (or is absent from work or quarantined).
						for (j = j1; (j < j2) && (!f); j++)
							f = ((abs(Hosts[j].inf) != InfStat_Dead) && (HOST_AGE_YEAR(j) >= P.CaseAbsentChildAgeCutoff) && ((Hosts[j].PlaceLinks[P.PlaceTypeNoAirNum - 1] < 0) || (HOST_ABSENT(j)) || (HOST_QUARANTINED(j))));

						//// so !f true if any household member EITHER: a) dead; b) a child; c) has a link to an office and not currently absent or quarantined.
						if (!f) //// so if either a) a household member is dead; b) a household member is a child requiring adult to stay home; c) a household member has links to office.
						{
							for (j = j1; (j < j2) & (!f); j++) /// loop again, checking whether household members not children needing supervision and are alive.
								if ((HOST_AGE_YEAR(j) >= P.CaseAbsentChildAgeCutoff) && (abs(Hosts[j].inf) != InfStat_Dead)) { k = j; f = 1; }
							if (f) //// so finally, if at least one member of household is alive and does not need supervision by an adult, amend absent start and stop times
							{
								Hosts[k].absent_start_time = ts + P.usCaseIsolationDelay;
								Hosts[k].absent_stop_time = ts + P.usCaseIsolationDelay + P.usCaseIsolationDuration;
								StateT[tn].cumAA++;
							}
						}
					}
				}
			}
		}

	//add contacts to digital contact tracing, but only if considering contact tracing, we are within the window of the policy and the detected case is a user
	if ((P.DoDigitalContactTracing) && (t >= adunit.DigitalContactTracingTimeStart) && (t < adunit.DigitalContactTracingTimeStart + P.DigitalContactTracingPolicyDuration) && (Hosts[ai].digitalContactTracingUser))
	{

		// allow for DCT to isolate index cases
		if ((P.DCTIsolateIndexCases) && (Hosts[ai].index_case_dct==0))//(Hosts[ai].digitalContactTraced == 0)&& - currently removed this condition as it would mean that someone already under isolation wouldn't have their isolation extended
		{
			ad = cell.adunit;
			//if (AdUnits[j].ndct < AdUnits[j].n)
			if(StateT[tn].ndct_queue[cell.adunit] < adunit.n)
			{
				//if we are isolating an index case, we set their infector as -1 in order to get the timings consistent.
				StateT[tn].dct_queue[cell.adunit][StateT[tn].ndct_queue[cell.adunit]++] = { ai,-1,ts };
			}
			else
			{
				fprintf(stderr, "No more space in queue! AdUnit: %i, ndct=%i, max queue length: %i\n", ad, AdUnits[j].ndct, AdUnits[ad].n);
				fprintf(stderr, "Error!\n");
			}
		}
		//currently commenting this out as household members will likely be picked up by household quarantine.
		//can add back in if needed, but would need to re-add a couple more local variables.

		//if(P.IncludeHouseholdDigitalContactTracing)
		//{
		//	//Then we want to find all their household and place group contacts to add to the contact tracing queue
		//	//Start with household contacts
		//	j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
		//	for (j = j1; j < j2; j++)
		//	{
		//		//if host is dead or the detected case, no need to add them to the list. They also need to be a user themselves
		//		if ((abs(Hosts[j].inf) != 5) && (j != ai) && (Hosts[j].digitalContactTracingUser) && (ranf_mt(tn)<P.ProportionDigitalContactsIsolate))
		//		{
		//			//add contact and detected infectious host to lists
		//			ad = Mcells[Hosts[j].mcell].adunit;
		//			if ((StateT[tn].ndct_queue[ad] < P.InfQueuePeakLength))
		//			{
		//				StateT[tn].dct_queue[ad][StateT[tn].ndct_queue[ad]++] = j;
		//				StateT[tn].contacts[ad][StateT[tn].ncontacts[ad]++] = ai;
		//			}
		//			else
		//			{
		//				fprintf(stderr, "No space left in queue! Thread: %i, AdUnit: %i\n", tn, ad);
		//			}
		//		}
		//	}
		//}
		//if(P.IncludePlaceGroupDigitalContactTracing)
		//{
		//	//then loop over place group contacts as well
		//	for (int i = 0; i < P.PlaceTypeNum; i++)
		//	{
		//		k = Hosts[ai].PlaceLinks[i];
		//		if (k >= 0)
		//		{
		//			//Find place group link
		//			m = Hosts[ai].PlaceGroupLinks[i];
		//			j1 = Places[i][k].group_start[m]; j2 = j1 + Places[i][k].group_size[m];
		//			for (j = j1; j < j2; j++)
		//			{
		//				h = Places[i][k].members[j];
		//				ad = Mcells[Hosts[h].mcell].adunit;
		//				//if host is dead or the detected case, no need to add them to the list. They also need to be a user themselves
		//				if ((abs(Hosts[h].inf) != 5) && (h != ai) && (Hosts[h].digitalContactTracingUser))// && (ranf_mt(tn)<P.ProportionDigitalContactsIsolate))
		//				{
		//					ad = Mcells[Hosts[h].mcell].adunit;
		//					if ((StateT[tn].ndct_queue[ad] < P.InfQueuePeakLength))
		//					{
		//						//PLEASE CHECK ALL THIS LOGIC CAREFULLY!
		//
		//						StateT[tn].dct_queue[ad][StateT[tn].ndct_queue[ad]++] = h;
		//						StateT[tn].contacts[ad][StateT[tn].ncontacts[ad]++] = ai; //keep a record of who the detected case was
		//					}
		//					else
		//					{
		//						fprintf(stderr, "No space left in queue! Thread: %i, AdUnit: %i\n", tn, ad);
		//					}
		//
		//				}
		//			}
		//		}
		//	}
		//}

	}

}

void DoCase(int ai, double t, unsigned short int ts, int tn) //// makes an infectious (but asymptomatic) person symptomatic. Called in IncubRecoverySweep (and DoInfect if P.DoOneGen)
{
	int j, k, f, j1, j2;
	Person* a;
	int age;

	age = HOST_AGE_GROUP(ai);
	if (age >= NUM_AGE_GROUPS) age = NUM_AGE_GROUPS - 1;
	a = Hosts + ai;
	if (a->inf == InfStat_InfectiousAlmostSymptomatic) //// if person latent/asymptomatically infected, but infectious
	{
		a->inf = InfStat_Case; //// make person symptomatic and infectious (i.e. a case)
		if (HOST_ABSENT(ai))
		{
			if (a->absent_stop_time < ts + P.usCaseAbsenteeismDelay + P.usCaseAbsenteeismDuration)
				a->absent_stop_time = ts + P.usCaseAbsenteeismDelay + P.usCaseAbsenteeismDuration;
		}
		else if((P.DoRealSymptWithdrawal)&&(P.DoPlaces))
		{
			a->absent_start_time = USHRT_MAX - 1;
			for (j = 0; j < P.PlaceTypeNum; j++)
				if ((a->PlaceLinks[j] >= 0) && (j != P.HotelPlaceType) && (!HOST_ABSENT(ai)) && (P.SymptPlaceTypeWithdrawalProp[j] > 0))
				{
					if ((P.SymptPlaceTypeWithdrawalProp[j] == 1) || (ranf_mt(tn) < P.SymptPlaceTypeWithdrawalProp[j]))
					{
						a->absent_start_time = ts + P.usCaseAbsenteeismDelay;
						a->absent_stop_time = ts + P.usCaseAbsenteeismDelay + P.usCaseAbsenteeismDuration;
						if (P.AbsenteeismPlaceClosure)
						{
							if ((t >= P.PlaceCloseTimeStart) && (!P.DoAdminTriggers) && (!P.DoGlobalTriggers))
							{
								for (int place_type = 0; place_type < P.PlaceTypeNum; place_type++)
									if ((place_type != P.HotelPlaceType) && (a->PlaceLinks[place_type] >= 0))
										DoPlaceClose(place_type, a->PlaceLinks[place_type], ts, tn, 0);

								j = P.PlaceTypeNum;
							}
						}
						if ((!HOST_QUARANTINED(ai)) && (Hosts[ai].PlaceLinks[P.PlaceTypeNoAirNum - 1] >= 0) && (HOST_AGE_YEAR(ai) >= P.CaseAbsentChildAgeCutoff))
							StateT[tn].cumAC++;
						/* This calculates adult absenteeism from work due to care of sick children. Note, children not at school not counted (really this should
						be fixed in population setup by having adult at home all the time for such kids. */
						if ((P.DoHouseholds) && (HOST_AGE_YEAR(ai) < P.CaseAbsentChildAgeCutoff))
						{
							if (!HOST_QUARANTINED(ai)) StateT[tn].cumACS++;
							if (Hosts[ai].ProbCare < P.CaseAbsentChildPropAdultCarers)
							{
								j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
								f = 0;
								for (int j3 = j1; (j3 < j2) && (!f); j3++)
									f = ((abs(Hosts[j3].inf) != InfStat_Dead) && (HOST_AGE_YEAR(j3) >= P.CaseAbsentChildAgeCutoff)
										&& ((Hosts[j3].PlaceLinks[P.PlaceTypeNoAirNum - 1] < 0)|| (HOST_ABSENT(j3)) || (HOST_QUARANTINED(j3))));
								if (!f)
								{
									for (int j3 = j1; (j3 < j2) && (!f); j3++)
										if ((HOST_AGE_YEAR(j3) >= P.CaseAbsentChildAgeCutoff) && (abs(Hosts[j3].inf) != InfStat_Dead)) { k = j3; f = 1; }
									if (f)
									{
										if (!HOST_ABSENT(k)) Hosts[k].absent_start_time = ts + P.usCaseIsolationDelay;
										Hosts[k].absent_stop_time = ts + P.usCaseIsolationDelay + P.usCaseIsolationDuration;
										StateT[tn].cumAA++;
									}
								}
							}
						}
					}
				} // End of if, and for(j)
		}

		//added some case detection code here: ggilani - 03/02/15
		if (Hosts[ai].detected == 1)
			//if ((P.ControlPropCasesId == 1) || (ranf_mt(tn) < P.ControlPropCasesId))
		{
			StateT[tn].cumDC++;
			StateT[tn].cumDC_adunit[Mcells[a->mcell].adunit]++;
			DoDetectedCase(ai, t, ts, tn);
			//add detection time

		}

		if (HOST_TREATED(ai)) Cells[Hosts[ai].pcell].cumTC++;
		StateT[tn].cumC++;
		StateT[tn].cumCa[age]++;
		StateT[tn].cumC_country[Mcells[Hosts[ai].mcell].country]++; //add to cumulative count of cases in that country: ggilani - 12/11/14
		StateT[tn].cumC_keyworker[a->keyworker]++;


		if (P.DoSeverity)
		{
			if (a->Severity_Final == Severity_Mild)
				DoMild(ai, tn);
			else
				DoILI(ai, tn); //// symptomatic cases either mild or ILI at symptom onset. SARI and Critical cases still onset with ILI.
		}
		if (P.DoAdUnits) StateT[tn].cumC_adunit[Mcells[a->mcell].adunit]++;
	}
}

void DoFalseCase(int ai, double t, unsigned short int ts, int tn)
{
	/* Arguably adult absenteeism to take care of sick kids could be included here, but then output absenteeism would not be 'excess' absenteeism */
	if ((P.ControlPropCasesId == 1) || (ranf_mt(tn) < P.ControlPropCasesId))
	{
		if ((!P.DoEarlyCaseDiagnosis) || (State.cumDC >= P.CaseOrDeathThresholdBeforeAlert)) StateT[tn].cumDC++;
		DoDetectedCase(ai, t, ts, tn);
	}
	StateT[tn].cumFC++;
}

void DoRecover(int ai, int tn, int run)
{
	int i, j, x, y;

	auto& person = *(Hosts + ai);
	if (person.inf == InfStat_InfectiousAsymptomaticNotCase || person.inf == InfStat_Case)
	{
		i = person.listpos;
		InfectiousToRecovered(person.pcell);
		auto& cell = Cells[person.pcell];
		j = cell.S + cell.L + cell.I;
		if (i < cell.S + cell.L + cell.I)
		{
			cell.susceptible[i] = cell.susceptible[j];
			Hosts[cell.susceptible[i]].listpos = i;
			person.listpos = j;
			cell.susceptible[j] = ai;
		}
		person.inf = (InfStat)(InfStat_Recovered * person.inf / abs(person.inf));
		if (P.DoAdUnits && P.OutputAdUnitAge)
			StateT[tn].prevInf_age_adunit[HOST_AGE_GROUP(ai)][Mcells[person.mcell].adunit]--;

		if (P.OutputBitmap)
		{
			if ((P.OutputBitmapDetected == 0) || ((P.OutputBitmapDetected == 1) && (Hosts[ai].detected == 1)))
			{
				x = ((int)(Households[person.hh].loc.x * P.scale.x)) - P.bmin.x;
				y = ((int)(Households[person.hh].loc.y * P.scale.y)) - P.bmin.y;
				if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
				{
					unsigned j = y * bmh->width + x;
					if (j < bmh->imagesize)
					{
#pragma omp atomic
						bmRecovered[j]++;
#pragma omp atomic
						bmInfected[j]--;
					}
				}
			}
		}
	}
	//else
	//fprintf(stderr, "\n ### %i %i  \n", ai, person.inf);
}

void DoDeath(int ai, int tn, int run)
{
	int i, x, y;
	auto& person = *(Hosts + ai);

	if ((person.inf == InfStat_InfectiousAsymptomaticNotCase || person.inf == InfStat_Case))
	{
		person.inf = (InfStat)(InfStat_Dead * person.inf / abs(person.inf));
		InfectiousToDeath(person.pcell);
		auto& cell = Cells[person.pcell];
		i = person.listpos;
		if (i < cell.S + cell.L + cell.I)
		{
			cell.susceptible[person.listpos] = cell.infected[cell.I];
			Hosts[cell.susceptible[person.listpos]].listpos = i;
			person.listpos = cell.S + cell.L + cell.I;
			cell.susceptible[person.listpos] = ai;
		}

		/*		person.listpos=-1; */
		StateT[tn].cumDa[HOST_AGE_GROUP(ai)]++;

		if (P.DoAdUnits)
		{
			StateT[tn].cumD_adunit[Mcells[person.mcell].adunit]++;
			if (P.OutputAdUnitAge) StateT[tn].prevInf_age_adunit[HOST_AGE_GROUP(ai)][Mcells[person.mcell].adunit]--;
		}
		if (P.OutputBitmap)
		{
			if ((P.OutputBitmapDetected == 0) || ((P.OutputBitmapDetected == 1) && (Hosts[ai].detected == 1)))
			{
				x = ((int)(Households[person.hh].loc.x * P.scale.x)) - P.bmin.x;
				y = ((int)(Households[person.hh].loc.y * P.scale.y)) - P.bmin.y;
				if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
				{
					unsigned j = y * bmh->width + x;
					if (j < bmh->imagesize)
					{
#pragma omp atomic
						bmRecovered[j]++;
#pragma omp atomic
						bmInfected[j]--;
					}
				}
			}
		}
	}
}

void DoTreatCase(int ai, unsigned short int ts, int tn)
{
	int x, y;

	if (State.cumT < P.TreatMaxCourses)
	{
#ifdef NO_TREAT_PROPH_CASES
		if (!HOST_TO_BE_TREATED(ai))
#endif
		{
			auto& host = Hosts[ai];
			auto& cell = Cells[host.pcell];
			auto& adunit = Mcells[host.mcell].adunit;
			auto& state = StateT[tn];

			host.treat_start_time = ts + ((unsigned short int) (P.TimeStepsPerDay * P.TreatDelayMean));
			host.treat_stop_time = ts + ((unsigned short int) (P.TimeStepsPerDay * (P.TreatDelayMean + P.TreatCaseCourseLength)));
			state.cumT++;
			if ((abs(host.inf) > InfStat_Susceptible) && (host.inf != InfStat_Dead_WasAsymp)) cell.cumTC++;
			state.cumT_keyworker[host.keyworker]++;
			if ((++host.num_treats) < 2) state.cumUT++;
			cell.tot_treat++;
			if (P.DoAdUnits) state.cumT_adunit[adunit]++;
			if (P.OutputBitmap)
			{
				x = ((int)(Households[host.hh].loc.x * P.scale.x)) - P.bmin.x;
				y = ((int)(Households[host.hh].loc.y * P.scale.y)) - P.bmin.y;
				if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
				{
					unsigned j = y * bmh->width + x;
					if (j < bmh->imagesize)
					{
#pragma omp atomic
						bmTreated[j]++;
					}
				}
			}
		}
	}
}

void DoProph(int ai, unsigned short int ts, int tn)
{
	//// almost identical to DoProphNoDelay, except unsurprisingly this function includes delay between timestep and start of treatment. Also increments StateT[tn].cumT_keyworker by 1 every time.
	int x, y;

	if (State.cumT < P.TreatMaxCourses)
	{
		auto& host = Hosts[ai];
		auto& cell = Cells[host.pcell];
		auto& adunit = Mcells[host.mcell].adunit;
		auto& state = StateT[tn];

		host.treat_start_time = ts + ((unsigned short int) (P.TimeStepsPerDay * P.TreatDelayMean));
		host.treat_stop_time = ts + ((unsigned short int) (P.TimeStepsPerDay * (P.TreatDelayMean + P.TreatProphCourseLength)));
		state.cumT++;
		state.cumT_keyworker[host.keyworker]++;
		if ((++host.num_treats) < 2) state.cumUT++;
		if (P.DoAdUnits) state.cumT_adunit[adunit]++;
#pragma omp critical (tot_treat)
		cell.tot_treat++;
		if (P.OutputBitmap)
		{
			x = ((int)(Households[host.hh].loc.x * P.scale.x)) - P.bmin.x;
			y = ((int)(Households[host.hh].loc.y * P.scale.y)) - P.bmin.y;
			if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
			{
				unsigned j = y * bmh->width + x;
				if (j < bmh->imagesize)
				{
#pragma omp atomic
					bmTreated[j]++;
				}
			}
		}
	}
}

void DoProphNoDelay(int ai, unsigned short int ts, int tn, int nc)
{
	int x, y;

	if (State.cumT < P.TreatMaxCourses)
	{
		auto& host = Hosts[ai];
		auto& cell = Cells[host.pcell];
		auto& adunit = Mcells[host.mcell].adunit;
		auto& state = StateT[tn];

		host.treat_start_time = ts;
		host.treat_stop_time = ts + ((unsigned short int) (P.TimeStepsPerDay * P.TreatProphCourseLength * nc));
		state.cumT += nc;
		state.cumT_keyworker[host.keyworker] += nc;
		if ((++host.num_treats) < 2) state.cumUT++;
		if (P.DoAdUnits) state.cumT_adunit[adunit] += nc;
#pragma omp critical (tot_treat)
		cell.tot_treat++;
		if (P.OutputBitmap)
		{
			x = ((int)(Households[host.hh].loc.x * P.scale.x)) - P.bmin.x;
			y = ((int)(Households[host.hh].loc.y * P.scale.y)) - P.bmin.y;
			if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
			{
				unsigned j = y * bmh->width + x;
				if (j < bmh->imagesize)
				{
#pragma omp atomic
					bmTreated[j]++;
				}
			}
		}
	}
}

void DoPlaceClose(int i, int j, unsigned short int ts, int tn, int DoAnyway)
{
	//// DoPlaceClose function called in TreatSweep (with arg DoAnyway = 1) and DoDetectedCase (with arg DoAnyway = 0).
	//// Basic pupose of this function is to change Places[i][j].close_start_time and Places[i][j].close_end_time, so that macro PLACE_CLOSED will return true.
	//// This will then scale peoples household, place, and spatial infectiousness and susceptibilities in function InfectSweep (but not in functions ini CalcInfSusc.cpp)

	int k, ai, j1, j2, l, f, f2;
	unsigned short trig;
	unsigned short int t_start, t_stop;
	unsigned short int t_old, t_new;
	auto& place = Places[i][j];
	auto& adunit = AdUnits[Mcells[place.mcell].adunit];

	f2 = 0;
	/*	if((j<0)||(j>=P.Nplace[i]))
			fprintf(stderr,"** %i %i *\n",i,j);
		else
	*/
	t_new = (unsigned short)(((double)ts) / P.TimeStepsPerDay);
	trig = 0;
	t_start = ts + ((unsigned short int) (P.TimeStepsPerDay * P.PlaceCloseDelayMean));
	if (P.DoInterventionDelaysByAdUnit)
	{
		t_stop = ts + ((unsigned short int) (P.TimeStepsPerDay * (P.PlaceCloseDelayMean + adunit.PlaceCloseDuration)));
	}
	else
	{
		t_stop = ts + ((unsigned short int) (P.TimeStepsPerDay * (P.PlaceCloseDelayMean + P.PlaceCloseDuration)));
	}
#pragma omp critical (closeplace)
	{
		//// close_start_time initialized to USHRT_MAX - 1.
		//// close_end_time initialized to zero in InitModel (so will pass this check on at least first call of this function).

		if (place.close_end_time < t_stop)
		{
			if ((!DoAnyway) && (place.control_trig < USHRT_MAX - 2))
			{
				place.control_trig++;
				if (P.AbsenteeismPlaceClosure)
				{
					t_old = place.AbsentLastUpdateTime;
					if (t_new >= t_old + P.MaxAbsentTime)
						for (l = 0; l < P.MaxAbsentTime; l++) place.Absent[l] = 0;
					else
						for (l = t_old; l < t_new; l++)place.Absent[l % P.MaxAbsentTime] = 0;
					for (l = t_new; l < t_new + P.usCaseAbsenteeismDuration / P.TimeStepsPerDay; l++) place.Absent[l % P.MaxAbsentTime]++;
					trig = place.Absent[t_new % P.MaxAbsentTime];
					place.AbsentLastUpdateTime = t_new;
					if ((P.PlaceCloseByAdminUnit) && (P.PlaceCloseAdunitPlaceTypes[i] > 0)
						&& (((double)trig) / ((double)place.n) > P.PlaceCloseCasePropThresh))
					{
						//fprintf(stderr,"** %i %i %i %i %lg ## ",i,j,(int) place.control_trig, (int) place.n,P.PlaceCloseCasePropThresh);
						if (adunit.place_close_trig < USHRT_MAX - 1) adunit.place_close_trig++;
					}
				}
				else
				{
					trig = place.control_trig;
					if ((P.PlaceCloseByAdminUnit) && (P.PlaceCloseAdunitPlaceTypes[i] > 0)
						&& (((double)place.control_trig) / ((double)place.n) > P.PlaceCloseCasePropThresh))
					{
						//fprintf(stderr,"** %i %i %i %i %lg ## ",i,j,(int) place.control_trig, (int) place.n,P.PlaceCloseCasePropThresh);
						if (adunit.place_close_trig < USHRT_MAX - 1) adunit.place_close_trig++;
					}
				}
			}
			if (place.control_trig < USHRT_MAX - 1) //// control_trig initialized to zero so this check will pass at least once
			{
				if (P.PlaceCloseFracIncTrig > 0)
					k = (((double)trig) / ((double)place.n) > P.PlaceCloseFracIncTrig);
				else
					k = (((int)trig) >= P.PlaceCloseIncTrig);
				if (((!P.PlaceCloseByAdminUnit) && (k)) || (DoAnyway))
				{
					if (P.DoPlaceCloseOnceOnly)
						place.control_trig = USHRT_MAX - 1;  //// Places only close once, and so this code block would not be entered again.
					else
						place.control_trig = 0;				//// otherwise reset the trigger.

					//// set close_start_time and close_end_time

					if (place.ProbClose >= P.PlaceCloseEffect[i]) //// if proportion of places of type i remaining open is 0 or if place is closed with prob 1 - PlaceCloseEffect[i]...
					{
						if (place.close_start_time > t_start) place.close_start_time = t_start;
						place.close_end_time = t_stop;
						f2 = 1; /// /set flag to true so next part of function used.
					}
					else
						place.close_start_time = place.close_end_time = t_stop; //// ... otherwise set start and end of closure to be the same, which will cause macro PLACE_CLOSED to always return false.
				}
			}
		}
	}

	if (f2)
	{
		if (P.DoRealSymptWithdrawal)
			for (k = 0; k < place.n; k++) //// loop over all people in place.
			{
				ai = place.members[k];
				if (((P.PlaceClosePropAttending[i] == 0) || (Hosts[ai].ProbAbsent >= P.PlaceClosePropAttending[i])))
				{
					if ((!HOST_ABSENT(ai)) && (!HOST_QUARANTINED(ai)) && (HOST_AGE_YEAR(ai) < P.CaseAbsentChildAgeCutoff)) //// if person is a child and neither absent nor quarantined
					{
						StateT[tn].cumAPCS++;
						if (Hosts[ai].ProbCare < P.CaseAbsentChildPropAdultCarers) //// if child needs adult supervision
						{
							j1 = Households[Hosts[ai].hh].FirstPerson; j2 = j1 + Households[Hosts[ai].hh].nh;
							if ((j1 < 0) || (j2 > P.PopSize)) fprintf(stderr, "++ %i %i %i (%i %i %i)##  ", ai, j1, j2, i, j, k);
							f = 0;

							//// in loop below, f true if any household member a) alive AND b) not a child AND c) has no links to workplace (or is absent from work or quarantined).
							for (l = j1; (l < j2) && (!f); l++)
								f = ((abs(Hosts[l].inf) != InfStat_Dead) && (HOST_AGE_YEAR(l) >= P.CaseAbsentChildAgeCutoff) && ((Hosts[l].PlaceLinks[P.PlaceTypeNoAirNum - 1] < 0) || (HOST_QUARANTINED(l))));
							if (!f) //// so !f true if there's no living adult household member who is not quarantined already or isn't a home-worker.
							{
								for (l = j1; (l < j2) && (!f); l++) //// loop over all household members of child this place: find the adults and ensure they're not dead...
									if ((HOST_AGE_YEAR(l) >= P.CaseAbsentChildAgeCutoff) && (abs(Hosts[l].inf) != InfStat_Dead))
									{
										if (Hosts[l].absent_start_time > t_start) Hosts[l].absent_start_time = t_start;
										if (Hosts[l].absent_stop_time < t_stop) Hosts[l].absent_stop_time = t_stop;
										StateT[tn].cumAPA++;
										f = 1;
									}
							}
						}
					}
					//#pragma omp critical (closeplace3)
					{
						///// finally amend absent start and stop times if they contradict place start and stop times.
						if (Hosts[ai].absent_start_time > t_start) Hosts[ai].absent_start_time = t_start;
						if (Hosts[ai].absent_stop_time < t_stop) Hosts[ai].absent_stop_time = t_stop;
					}
					if ((HOST_AGE_YEAR(ai) >= P.CaseAbsentChildAgeCutoff) && (Hosts[ai].PlaceLinks[P.PlaceTypeNoAirNum - 1] >= 0)) StateT[tn].cumAPC++;
				}
			}
	}
}


void DoPlaceOpen(int i, int j, unsigned short int ts, int tn)
{
	int k, ai, j1, j2, l, f;

#pragma omp critical (openplace)
	{
		auto& place = Places[i][j];
		if (ts < place.close_end_time)
		{
			if (P.DoRealSymptWithdrawal)
				for (k = 0; k < place.n; k++)
				{
					const auto ai = place.members[k];
					auto& person = Hosts[ai];
					if (person.absent_stop_time == place.close_end_time) person.absent_stop_time = ts;
					if (person.ProbCare < P.CaseAbsentChildPropAdultCarers) //// if child needs adult supervision
					{
						if ((HOST_AGE_YEAR(ai) < P.CaseAbsentChildAgeCutoff) && (!HOST_QUARANTINED(ai)))
						{
							j1 = Households[person.hh].FirstPerson;
							j2 = j1 + Households[person.hh].nh;
							f = 0;
							for (l = j1; (l < j2) && (!f); l++)
								f = ((abs(Hosts[l].inf) != InfStat_Dead) && (HOST_AGE_YEAR(l) >= P.CaseAbsentChildAgeCutoff) && ((Hosts[l].PlaceLinks[P.PlaceTypeNoAirNum - 1] < 0) || (HOST_QUARANTINED(l))));
							if (!f)
							{
								for (l = j1; (l < j2) && (!f); l++)
									if ((HOST_AGE_YEAR(l) >= P.CaseAbsentChildAgeCutoff) && (abs(Hosts[l].inf) != InfStat_Dead) && (HOST_ABSENT(l)))
									{
										if (Hosts[l].absent_stop_time == place.close_end_time) Hosts[l].absent_stop_time = ts;
									}
							}
						}
					}
				}
			place.close_end_time = ts;
		}
	}
}

void DoVacc(int ai, unsigned short int ts)
{
	int x, y;
	bool cumV_OK = false;

	if ((HOST_TO_BE_VACCED(ai)) || (Hosts[ai].inf < InfStat_InfectiousAlmostSymptomatic) || (Hosts[ai].inf >= InfStat_Dead_WasAsymp))
		return;
#pragma omp critical (state_cumV)
	if (State.cumV < P.VaccMaxCourses)
	{
		cumV_OK = true;
		State.cumV++;
	}
	if (cumV_OK)
	{
		Hosts[ai].vacc_start_time = ts + ((unsigned short int) (P.TimeStepsPerDay * P.VaccDelayMean));

		if (P.VaccDosePerDay >= 0)
		{
#pragma omp critical (state_cumV_daily)
			State.cumV_daily++;
		}
#pragma omp critical (tot_vacc)
		Cells[Hosts[ai].pcell].tot_vacc++;
		if (P.OutputBitmap)
		{
			x = ((int)(Households[Hosts[ai].hh].loc.x * P.scale.x)) - P.bmin.x;
			y = ((int)(Households[Hosts[ai].hh].loc.y * P.scale.y)) - P.bmin.y;
			if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
			{
				unsigned j = y * bmh->width + x;
				if (j < bmh->imagesize)
				{
#pragma omp atomic
					bmTreated[j]++;
				}
			}
		}
	}
}

void DoVaccNoDelay(int ai, unsigned short int ts)
{
	int x, y;
	bool cumVG_OK = false;

	if ((HOST_TO_BE_VACCED(ai)) || (Hosts[ai].inf < InfStat_InfectiousAlmostSymptomatic) || (Hosts[ai].inf >= InfStat_Dead_WasAsymp))
		return;
#pragma omp critical (state_cumVG)
	if (State.cumVG < P.VaccMaxCourses)
	{
		cumVG_OK = true;
		State.cumVG++;
	}
	if (cumVG_OK)
	{
		Hosts[ai].vacc_start_time = ts;
		if (P.VaccDosePerDay >= 0)
		{
#pragma omp critical (state_cumV_daily)
			State.cumVG_daily++;
		}
#pragma omp critical (tot_vacc)
		Cells[Hosts[ai].pcell].tot_vacc++;
		if (P.OutputBitmap)
		{
			x = ((int)(Households[Hosts[ai].hh].loc.x * P.scale.x)) - P.bmin.x;
			y = ((int)(Households[Hosts[ai].hh].loc.y * P.scale.y)) - P.bmin.y;
			if ((x >= 0) && (x < P.b.width) && (y >= 0) && (y < P.b.height))
			{
				unsigned j = y * bmh->width + x;
				if (j < bmh->imagesize)
				{
#pragma omp atomic
					bmTreated[j]++;
				}
			}
		}
	}
}

///// Change person status functions (e.g. change person from susceptible to latently infected).
Severity ChooseFinalDiseaseSeverity(int AgeGroup, int tn)
{
	Severity DiseaseSeverity;
	double x;

	// assume normalised props

	x = ranf_mt(tn);
	if (x < P.Prop_ILI_ByAge[AgeGroup]) DiseaseSeverity = Severity_ILI;
	else if (x < P.Prop_ILI_ByAge[AgeGroup] + P.Prop_SARI_ByAge[AgeGroup]) DiseaseSeverity = Severity_SARI;
	else if (x < P.Prop_ILI_ByAge[AgeGroup] + P.Prop_SARI_ByAge[AgeGroup] + P.Prop_Critical_ByAge[AgeGroup]) DiseaseSeverity = Severity_Critical;
	else DiseaseSeverity = Severity_Mild;
	return DiseaseSeverity;
}

unsigned short int ChooseFromICDF(double *ICDF, double Mean, int tn)
{
	unsigned short int Value;
	int i;
	double q, ti;

	i = (int)floor(q = ranf_mt(tn) * CDF_RES); //// note q defined here as well as i.
	q -= ((double)i); //// remainder
	ti = -Mean * log(q * ICDF[i + 1] + (1.0 - q) * ICDF[i]); //// weighted average (sort of) between quartile values from CDF_RES. logged as it was previously exponentiated in ReadParams. Minus as exp(-cdf) was done in ReadParaams. Sort of
	Value = (unsigned short int) floor(0.5 + (ti * P.TimeStepsPerDay));

	return Value;
}

void SusceptibleToRecovered(int cellIndex)
{
	Cells[cellIndex].S--;
	Cells[cellIndex].R++;
	Cells[cellIndex].latent--;
	Cells[cellIndex].infected--;
}

void SusceptibleToLatent(int cellIndex)
{
	Cells[cellIndex].S--; 
	Cells[cellIndex].L++;			//// number of latently infected people increases by one.
	Cells[cellIndex].latent--;		//// pointer to latent in that cell decreased.
}


void LatentToInfectious(int cellIndex)
{
	Cells[cellIndex].L--;		//// one fewer person latently infected.
	Cells[cellIndex].I++;		//// one more infectious person.
	Cells[cellIndex].infected--; //// first infected person is now one index earlier in array.
}

void InfectiousToRecovered(int cellIndex)
{
	Cells[cellIndex].I--; //// one less infectious person
	Cells[cellIndex].R++; //// one more recovered person
}


void InfectiousToDeath(int cellIndex)
{
	Cells[cellIndex].I--; //// one less infectious person
	Cells[cellIndex].D++; //// one more dead person
}

