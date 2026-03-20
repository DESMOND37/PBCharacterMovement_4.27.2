// Copyright Project Borealis
// ============================================================
// UE4.27.2 Port — Changes summary (v3.1.0 → UE4.27.2):
//
// REMOVED (UE5-only):
//   - #include UE_INLINE_GENERATED_CPP_BY_NAME(...)
//   - HasCustomGravity() / GetGravitySpaceZ() / SetGravitySpaceZ()
//   - RotateGravityToWorld() / GetWorldToGravityTransform() / GetGravityDirection()
//   - JumpCurrentCountPreJump  → replaced with JumpCurrentCount
//   - bDontFallBelowJumpZVelocityDuringJump
//   - MaxJumpApexAttemptsPerSimulation property (used via local constexpr instead)
//   - bBasedMovementIgnorePhysicsBase
//   - TObjectPtr<> → raw pointer
//   - UE_KINDA_SMALL_NUMBER  → KINDA_SMALL_NUMBER
//   - UE_PI                  → PI
//   - FVector::FReal         → float
//   - SetCrouchedHalfHeight() / GetCrouchedHalfHeight()  → CrouchedHalfHeight (direct member)
//
// ADDED FROM 2.0.1:
//   - PhysFalling() override for correct apex sub-stepping on UE4 physics
//   - Engine version guards for conditional includes
//   - Direct CrouchedHalfHeight member assignment (UE4 API)
//
// RETAINED FROM 3.1.0:
//   - Air jump / air dash logic (DoJump)
//   - Edge friction system
//   - Crouch sliding system
//   - Improved MoveUpdatedComponentImpl (box sweep vs line trace)
//   - Improved OnMovementModeChanged (HasEverLanded guard)
//   - UpdateSurfaceFriction with OldBase caching
//   - TraceCharacterFloor fix (subtracts capsule half-height for correct trace depth)
//   - GetFallSpeed (bAfterLand)
//   - ApplyDownwardForce
// ============================================================

#include "Character/PBPlayerMovement.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Sound/SoundCue.h"

#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#endif

#include "Sound/PBMoveStepSound.h"

static TAutoConsoleVariable<int32> CVarShowPos(TEXT("cl.ShowPos"), 0,
	TEXT("Show position and movement information.\n"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarAlwaysApplyFriction(TEXT("move.AlwaysApplyFriction"), 0,
	TEXT("Apply friction, even in air.\n"), ECVF_Default);

DECLARE_CYCLE_STAT(TEXT("Char StepUp"),    STAT_CharStepUp,    STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysFalling"), STAT_CharPhysFalling, STATGROUP_Character);

// MAGIC NUMBERS
constexpr float JumpVelocity         = 266.7f;
const float MAX_STEP_SIDE_Z          = 0.08f;
const float VERTICAL_SLOPE_NORMAL_Z  = 0.001f;

// UE4 port: MaxJumpApexAttemptsPerSimulation is a UE5-only public property.
// In UE4.27 the same behavior is achieved with this local constant.
static constexpr int32 PB_MAX_JUMP_APEX_ATTEMPTS = 4;

#ifndef USE_HL2_GRAVITY
#define USE_HL2_GRAVITY 1
#endif

// ============================================================
// Constructor
// ============================================================
UPBPlayerMovement::UPBPlayerMovement()
{
	AirControl                       = 1.0f;
	AirControlBoostMultiplier        = 0.0f;
	AirControlBoostVelocityThreshold = 0.0f;
	MaxAcceleration                  = 857.25f;

	WalkSpeed   = 285.75f;
	RunSpeed    = 361.9f;
	SprintSpeed = 609.6f;
	MaxWalkSpeed = RunSpeed;

	GroundAccelerationMultiplier = 10.0f;
	AirAccelerationMultiplier    = 10.0f;
	AirSpeedCap                  = 57.15f;
	AirSlideSpeedCap             = 57.15f;

	GroundFriction             = 4.0f;
	BrakingFriction            = 4.0f;
	SurfaceFriction            = 1.0f;
	bUseSeparateBrakingFriction = false;

	EdgeFrictionMultiplier         = 2.0f;
	EdgeFrictionHeight             = 64.77f;
	EdgeFrictionDist               = 30.48f;
	bEdgeFrictionOnlyWhenBraking   = false;
	bEdgeFrictionAlwaysWhenCrouching = false;

	BrakingFrictionFactor = 1.0f;
	BrakingSubStepTime    = 1.0f / 66.0f;
	MaxSimulationTimeStep = 1.0f / 66.0f;
	MaxSimulationIterations = 25;
	// UE4 port: MaxJumpApexAttemptsPerSimulation does not exist in UE4.27 as a property.
	// It is handled via the local PB_MAX_JUMP_APEX_ATTEMPTS constant in PhysFalling().

	FallingLateralFriction      = 0.0f;
	BrakingDecelerationFalling   = 0.0f;
	BrakingDecelerationFlying    = 190.5f;
	BrakingDecelerationSwimming  = 190.5f;
	BrakingDecelerationWalking   = 190.5f;

	MaxStepHeight    = 34.29f;
	DefaultStepHeight = MaxStepHeight;
	MinStepHeight    = 10.0f;
	StepDownHeightFraction = 0.9f;

	PerchRadiusThreshold  = 0.5f;
	PerchAdditionalHeight = 0.0f;

	JumpZVelocity     = 304.8f;
	JumpOffJumpZFactor = 0.0f;

	bShowPos    = false;
	bOnLadder   = false;
	OffLadderTicks = LADDER_MOUNT_TIMEOUT;
	LadderSpeed = 381.0f;

	SpeedMultMin      = SprintSpeed * 1.7f;
	SpeedMultMax      = SprintSpeed * 2.5f;
	DefaultSpeedMultMin = SpeedMultMin;
	DefaultSpeedMultMax = SpeedMultMax;

	bBrakingFrameTolerated  = true;
	BrakingWindowTimeElapsed = 0.0f;
	BrakingWindow           = 0.015f;

	// UE4 port: In UE4.27, CrouchedHalfHeight is a public float member.
	// SetCrouchedHalfHeight() does NOT exist in UE4 — use direct assignment.
	CrouchedHalfHeight        = 34.29f;
	MaxWalkSpeedCrouched      = RunSpeed * 0.33333333f;
	bCanWalkOffLedgesWhenCrouching = true;
	CrouchTime     = MOVEMENT_DEFAULT_CROUCHTIME;
	UncrouchTime   = MOVEMENT_DEFAULT_UNCROUCHTIME;
	CrouchJumpTime = MOVEMENT_DEFAULT_CROUCHJUMPTIME;
	UncrouchJumpTime = MOVEMENT_DEFAULT_UNCROUCHJUMPTIME;

	SetWalkableFloorZ(0.7f);
	DefaultWalkableFloorZ = GetWalkableFloorZ();
	AxisSpeedLimit        = 6667.5f;

	StandingDownwardForceScale = 1.0f;
	bPushForceUsingZOffset     = false;
	PushForcePointZOffsetFactor = -0.66f;
	bScalePushForceToVelocity  = true;
	bPushForceScaledToMass     = false;
	bTouchForceScaledToMass    = false;
	Mass = 85.0f;

	bUseControllerDesiredRotation = false;
	bUseFlatBaseForFloorChecks    = true;

	NavAgentProps.bCanCrouch = true;
	NavAgentProps.bCanJump   = true;
	NavAgentProps.bCanFly    = true;

	bShouldCrouchSlide             = false;
	CrouchSlideBoostTime           = 0.1f;
	CrouchSlideBoostMultiplier     = 1.5f;
	CrouchSlideSpeedRequirementMultiplier = 0.9f;
	MinCrouchSlideBoost            = SprintSpeed * CrouchSlideBoostMultiplier;
	MaxCrouchSlideVelocityBoost    = 6.0f;
	MinCrouchSlideVelocityBoost    = 2.7f;
	CrouchSlideBoostSlopeFactor    = 2.7f;
	CrouchSlideCooldown            = 1.0f;

#if USE_HL2_GRAVITY
	GravityScale = DesiredGravity / UPhysicsSettings::Get()->DefaultGravityZ;
#endif

	bMaintainHorizontalGroundVelocity = true;
	bAlwaysCheckFloor                 = true;
	bIgnoreBaseRotation               = true;
	// UE4 port: bBasedMovementIgnorePhysicsBase is UE5-only, removed.

	bEnablePhysicsInteraction = true;
	RepulsionForce            = 1.314f;
	MaxTouchForce             = 100.0f;
	InitialPushForceFactor    = 10.0f;
	PushForceFactor           = 100000.0f;

	Buoyancy                              = 0.99f;
	bAllowPhysicsRotationDuringAnimRootMotion = true;
	RequestedVelocity                     = FVector::ZeroVector;
	bEnableServerDualMoveScopedMovementUpdates = true;
}

// ============================================================
// InitializeComponent
// ============================================================
void UPBPlayerMovement::InitializeComponent()
{
	Super::InitializeComponent();
	PBPlayerCharacter = Cast<APBPlayerCharacter>(CharacterOwner);

	MaxWalkSpeed = RunSpeed;
	if (SpeedMultMin == DefaultSpeedMultMin)
	{
		SpeedMultMin = SprintSpeed * 1.7f;
	}
	if (SpeedMultMax == DefaultSpeedMultMax)
	{
		SpeedMultMax = SprintSpeed * 2.5f;
	}
	DefaultStepHeight    = MaxStepHeight;
	DefaultWalkableFloorZ = GetWalkableFloorZ();
}

void UPBPlayerMovement::OnRegister()
{
	Super::OnRegister();
	const bool bIsReplay = (GetWorld() && GetWorld()->IsPlayingReplay());
	if (!bIsReplay && GetNetMode() == NM_ListenServer)
	{
		NetworkSmoothingMode = ENetworkSmoothingMode::Linear;
	}
}

// ============================================================
// TickComponent
// ============================================================
void UPBPlayerMovement::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PlayMoveSound(DeltaTime);

	if (bHasDeferredMovementMode)
	{
		SetMovementMode(DeferredMovementMode);
		bHasDeferredMovementMode = false;
	}

	if (UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	if (bShowPos || CVarShowPos.GetValueOnGameThread() != 0)
	{
		const FVector   Position = UpdatedComponent->GetComponentLocation();
		const FRotator  Rotation = CharacterOwner->GetControlRotation();
		const float     Speed    = Velocity.Size();
		GEngine->AddOnScreenDebugMessage(1, 1.0f, FColor::Green, FString::Printf(TEXT("pos: %.02f %.02f %.02f"), Position.X, Position.Y, Position.Z));
		GEngine->AddOnScreenDebugMessage(2, 1.0f, FColor::Green, FString::Printf(TEXT("ang: %.02f %.02f %.02f"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
		GEngine->AddOnScreenDebugMessage(3, 1.0f, FColor::Green, FString::Printf(TEXT("vel:  %.02f"), Speed));
	}

	if (RollAngle != 0 && RollSpeed != 0 && GetPBCharacter()->GetController())
	{
		FRotator ControlRotation = GetPBCharacter()->GetController()->GetControlRotation();
		ControlRotation.Roll = GetCameraRoll();
		GetPBCharacter()->GetController()->SetControlRotation(ControlRotation);
	}

	if (IsMovingOnGround())
	{
		if (!bBrakingFrameTolerated)
		{
			BrakingWindowTimeElapsed += DeltaTime;
			if (BrakingWindowTimeElapsed >= BrakingWindow)
			{
				bBrakingFrameTolerated = true;
			}
		}
	}
	else
	{
		bBrakingFrameTolerated   = false;
		BrakingWindowTimeElapsed = 0.0f;
	}
	bCrouchFrameTolerated = IsCrouching();
}

// ============================================================
// DoJump — UE4 port of 3.1.0 with custom gravity API removed
// ============================================================
bool UPBPlayerMovement::DoJump(bool bClientSimulation)
{
	if (!bCheatFlying && CharacterOwner && CharacterOwner->CanJump())
	{
		// UE4 port: replaced GetGravitySpaceZ(PlaneConstraintNormal) with PlaneConstraintNormal.Z
		if (!bConstrainToPlane || !FMath::IsNearlyEqual(FMath::Abs(PlaneConstraintNormal.Z), 1.f))
		{
			// Air jump / dash (from 3.1.0, multi-jump support)
			// UE4 port: JumpCurrentCountPreJump doesn't exist in UE4 — use JumpCurrentCount
			const int32 NewJumps = CharacterOwner->JumpCurrentCount + 1;
			if (IsFalling() && GetCharacterOwner()->JumpMaxCount > 1 && NewJumps <= GetCharacterOwner()->JumpMaxCount)
			{
				if (bAirJumpResetsHorizontal)
				{
					Velocity.X = 0.0f;
					Velocity.Y = 0.0f;
				}
				FVector InputVector = GetCharacterOwner()->GetPendingMovementInputVector() + GetLastInputVector();
				InputVector = InputVector.GetSafeNormal2D();
				Velocity += InputVector * GetMaxAcceleration() * AirJumpDashMagnitude;
				OnAirJump(NewJumps);
			}

			// UE4 port: Removed HasCustomGravity() branch (UE5 Custom Gravity API).
			// Standard gravity path: gravity is always along -Z in UE4.
			// UE4 port: FVector::FReal does not exist in UE4 — use float.
			if (Velocity.Z < 0.0f)
			{
				Velocity.Z = 0.0f;
			}
			Velocity.Z += FMath::Max<float>(Velocity.Z, JumpZVelocity);

			SetMovementMode(MOVE_Falling);
			return true;
		}
	}
	return false;
}

// ============================================================
// GetFallSpeed
// ============================================================
float UPBPlayerMovement::GetFallSpeed(bool bAfterLand)
{
	// UE4 port: Removed HasCustomGravity() branch.
	FVector FallVelocity = Velocity;
	if (bAfterLand)
	{
		const float GravityStep = GetGravityZ() * GetWorld()->GetDeltaSeconds() * 0.5f;
		FallVelocity.Z += GravityStep;
	}
	return -FallVelocity.Z;
}

// ============================================================
// PhysFalling — from 2.0.1 (UE4-native implementation)
// This override is needed in UE4 to ensure correct jump apex
// sub-stepping behavior. UE5 (3.0+) removed it as redundant.
// ============================================================
void UPBPlayerMovement::PhysFalling(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysFalling);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector FallAcceleration = GetFallingLateralAcceleration(deltaTime);
	FallAcceleration.Z = 0.f;
	const bool bHasLimitedAirControl = ShouldLimitAirControl(deltaTime, FallAcceleration);

	// UE4 port: NumJumpApexAttempts is already a member of UCharacterMovementComponent
	// in UE4 (it was refactored into a local variable in UE5's rewrite).
	// We reset it here at the start of each PhysFalling call, exactly as UE4 does internally.
	// MaxJumpApexAttemptsPerSimulation is not a UE4 property; PB_MAX_JUMP_APEX_ATTEMPTS is used instead.
	NumJumpApexAttempts = 0;

	float remainingTime = deltaTime;
	while ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations))
	{
		Iterations++;
		float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FQuat PawnRotation  = UpdatedComponent->GetComponentQuat();
		bJustTeleported = false;

		const FVector OldVelocityWithRootMotion = Velocity;
		RestorePreAdditiveRootMotionVelocity();
		const FVector OldVelocity = Velocity;

		const float MaxDecel = GetMaxBrakingDeceleration();
		if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			TGuardValue<FVector> RestoreAcceleration(Acceleration, FallAcceleration);
			Velocity.Z = 0.f;
			CalcVelocity(timeTick, FallingLateralFriction, false, MaxDecel);
			Velocity.Z = OldVelocity.Z;
		}

		const FVector Gravity(0.f, 0.f, GetGravityZ());
		float GravityTime = timeTick;

		bool bEndingJumpForce = false;
		if (CharacterOwner->JumpForceTimeRemaining > 0.0f)
		{
			const float JumpForceTime = FMath::Min(CharacterOwner->JumpForceTimeRemaining, timeTick);
			GravityTime = bApplyGravityWhileJumping ? timeTick : FMath::Max(0.0f, timeTick - JumpForceTime);
			CharacterOwner->JumpForceTimeRemaining -= JumpForceTime;
			if (CharacterOwner->JumpForceTimeRemaining <= 0.0f)
			{
				CharacterOwner->ResetJumpState();
				bEndingJumpForce = true;
			}
		}

		Velocity = NewFallVelocity(Velocity, Gravity, GravityTime);

		// Jump apex sub-stepping
		if (OldVelocity.Z > 0.f && Velocity.Z <= 0.f && NumJumpApexAttempts < PB_MAX_JUMP_APEX_ATTEMPTS)
		{
			const FVector DerivedAccel = (Velocity - OldVelocity) / timeTick;
			if (!FMath::IsNearlyZero(DerivedAccel.Z))
			{
				const float TimeToApex    = -OldVelocity.Z / DerivedAccel.Z;
				const float ApexTimeMin   = 0.0001f;
				if (TimeToApex >= ApexTimeMin && TimeToApex < timeTick)
				{
					const FVector ApexVelocity = OldVelocity + DerivedAccel * TimeToApex;
					Velocity   = ApexVelocity;
					Velocity.Z = 0.f;
					remainingTime += (timeTick - TimeToApex);
					timeTick = TimeToApex;
					Iterations--;
					NumJumpApexAttempts++;
				}
			}
		}

		ApplyRootMotionToVelocity(timeTick);

		if (bNotifyApex && (Velocity.Z < 0.f))
		{
			bNotifyApex = false;
			NotifyJumpApex();
		}

		FVector Adjusted = 0.5f * (OldVelocityWithRootMotion + Velocity) * timeTick;

		if (bEndingJumpForce && !bApplyGravityWhileJumping)
		{
			const float NonGravityTime = FMath::Max(0.f, timeTick - GravityTime);
			Adjusted = (OldVelocityWithRootMotion * NonGravityTime) + (0.5f * (OldVelocityWithRootMotion + Velocity) * GravityTime);
		}

		FHitResult Hit(1.f);
		SafeMoveUpdatedComponent(Adjusted, PawnRotation, true, Hit);

		if (!HasValidData()) { return; }

		float LastMoveTimeSlice     = timeTick;
		float subTimeTickRemaining  = timeTick * (1.f - Hit.Time);

		if (IsSwimming())
		{
			remainingTime += subTimeTickRemaining;
			StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
			return;
		}
		else if (Hit.bBlockingHit)
		{
			if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				remainingTime += subTimeTickRemaining;
				ProcessLanded(Hit, remainingTime, Iterations);
				return;
			}
			else
			{
				if (!Hit.bStartPenetrating && ShouldCheckForValidLandingSpot(timeTick, Adjusted, Hit))
				{
					const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
					FFindFloorResult FloorResult;
					FindFloor(PawnLocation, FloorResult, false);
					if (FloorResult.IsWalkableFloor() && IsValidLandingSpot(PawnLocation, FloorResult.HitResult))
					{
						remainingTime += subTimeTickRemaining;
						ProcessLanded(FloorResult.HitResult, remainingTime, Iterations);
						return;
					}
				}

				HandleImpact(Hit, LastMoveTimeSlice, Adjusted);
				if (!HasValidData() || !IsFalling()) { return; }

				FVector VelocityNoAirControl = OldVelocity;
				FVector AirControlAccel      = Acceleration;
				if (bHasLimitedAirControl)
				{
					{
						TGuardValue<FVector> RestoreAcceleration(Acceleration, FVector::ZeroVector);
						TGuardValue<FVector> RestoreVelocity(Velocity, OldVelocity);
						Velocity.Z = 0.f;
						CalcVelocity(timeTick, FallingLateralFriction, false, MaxDecel);
						VelocityNoAirControl = FVector(Velocity.X, Velocity.Y, OldVelocity.Z);
						VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, Gravity, GravityTime);
					}
					AirControlAccel       = (Velocity - VelocityNoAirControl) / timeTick;
					const FVector AirControlDeltaV = LimitAirControl(LastMoveTimeSlice, AirControlAccel, Hit, false) * LastMoveTimeSlice;
					Adjusted = (VelocityNoAirControl + AirControlDeltaV) * LastMoveTimeSlice;
				}

				const FVector OldHitNormal       = Hit.Normal;
				const FVector OldHitImpactNormal  = Hit.ImpactNormal;
				FVector Delta     = ComputeSlideVector(Adjusted, 1.f - Hit.Time, OldHitNormal, Hit);
				FVector DeltaStep = ComputeSlideVector(Velocity * timeTick, 1.f - Hit.Time, OldHitNormal, Hit);

				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
				{
					const FVector NewVelocity = (DeltaStep / subTimeTickRemaining);
					Velocity = HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocityWithIgnoreZAccumulate()
						? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
				}

				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && (Delta | Adjusted) > 0.f)
				{
					SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
					if (Hit.bBlockingHit)
					{
						LastMoveTimeSlice    = subTimeTickRemaining;
						subTimeTickRemaining = subTimeTickRemaining * (1.f - Hit.Time);

						if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
						{
							remainingTime += subTimeTickRemaining;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}

						HandleImpact(Hit, LastMoveTimeSlice, Delta);
						if (!HasValidData() || !IsFalling()) { return; }

						if (bHasLimitedAirControl && Hit.Normal.Z > VERTICAL_SLOPE_NORMAL_Z)
						{
							const FVector LastMoveNoAirControl = VelocityNoAirControl * LastMoveTimeSlice;
							Delta = ComputeSlideVector(LastMoveNoAirControl, 1.f, OldHitNormal, Hit);
						}

						FVector PreTwoWallDelta = Delta;
						TwoWallAdjust(Delta, Hit, OldHitNormal);

						if (bHasLimitedAirControl)
						{
							const FVector AirControlDeltaV = LimitAirControl(subTimeTickRemaining, AirControlAccel, Hit, false) * subTimeTickRemaining;
							if (FVector::DotProduct(AirControlDeltaV, OldHitNormal) > 0.f)
							{
								Delta += (AirControlDeltaV * subTimeTickRemaining);
							}
						}

						if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
						{
							const FVector NewVelocity = (Delta / subTimeTickRemaining);
							Velocity = HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocityWithIgnoreZAccumulate()
								? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
						}

						const bool bDitch = ((OldHitImpactNormal.Z > 0.f) && (Hit.ImpactNormal.Z > 0.f) &&
							(FMath::Abs(Delta.Z) <= KINDA_SMALL_NUMBER) && ((Hit.ImpactNormal | OldHitImpactNormal) < 0.f));

						SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
						if (Hit.Time == 0.f)
						{
							FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).GetSafeNormal2D();
							if (SideDelta.IsNearlyZero())
							{
								SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0).GetSafeNormal();
							}
							SafeMoveUpdatedComponent(SideDelta, PawnRotation, true, Hit);
						}

						if (bDitch || IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit) || Hit.Time == 0.f)
						{
							remainingTime = 0.f;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}
						else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && OldHitImpactNormal.Z >= GetWalkableFloorZ())
						{
							const FVector PawnLocation  = UpdatedComponent->GetComponentLocation();
							const float ZMovedDist      = FMath::Abs(PawnLocation.Z - OldLocation.Z);
							const float MovedDist2DSq   = (PawnLocation - OldLocation).SizeSquared2D();
							if (ZMovedDist <= 0.2f * timeTick && MovedDist2DSq <= 4.f * timeTick)
							{
								Velocity.X += 0.25f * GetMaxSpeed() * (RandomStream.FRand() - 0.5f);
								Velocity.Y += 0.25f * GetMaxSpeed() * (RandomStream.FRand() - 0.5f);
								Velocity.Z  = FMath::Max<float>(JumpZVelocity * 0.25f, 1.f);
								Delta = Velocity * timeTick;
								SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
							}
						}
					}
				}
			}
		}

		if (Velocity.SizeSquared2D() <= KINDA_SMALL_NUMBER * 10.f)
		{
			Velocity.X = 0.f;
			Velocity.Y = 0.f;
		}
	}
}

// ============================================================
// Surface / Slope overrides
// ============================================================
void UPBPlayerMovement::TwoWallAdjust(FVector& Delta, const FHitResult& Hit, const FVector& OldHitNormal) const
{
	Super::TwoWallAdjust(Delta, Hit, OldHitNormal);
}

float UPBPlayerMovement::SlideAlongSurface(const FVector& Delta, float Time, const FVector& Normal, FHitResult& Hit, bool bHandleImpact)
{
	return Super::SlideAlongSurface(Delta, Time, Normal, Hit, bHandleImpact);
}

FVector UPBPlayerMovement::ComputeSlideVector(const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) const
{
	return Super::ComputeSlideVector(Delta, Time, Normal, Hit);
}

FVector UPBPlayerMovement::HandleSlopeBoosting(const FVector& SlideResult, const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) const
{
	if (IsOnLadder() || bCheatFlying)
	{
		return Super::HandleSlopeBoosting(SlideResult, Delta, Time, Normal, Hit);
	}
	const float WallAngle = FMath::Abs(Hit.ImpactNormal.Z);
	FVector ImpactNormal  = Normal;
	if (!(WallAngle <= VERTICAL_SLOPE_NORMAL_Z || WallAngle == 1.0f))
	{
		if (Hit.ImpactNormal.Z <= ImpactNormal.Z && Delta.Z <= 0.0f)
		{
			ImpactNormal = Hit.ImpactNormal;
		}
	}
	if (bConstrainToPlane)
	{
		ImpactNormal = ConstrainNormalToPlane(ImpactNormal);
	}
	const float BounceCoefficient = 1.0f + BounceMultiplier * (1.0f - SurfaceFriction);
	return (Delta - BounceCoefficient * Delta.ProjectOnToNormal(ImpactNormal)) * Time;
}

bool UPBPlayerMovement::ShouldCatchAir(const FFindFloorResult& OldFloor, const FFindFloorResult& NewFloor)
{
	// Step-down check
	const float HeightDiff = NewFloor.HitResult.ImpactPoint.Z - OldFloor.HitResult.ImpactPoint.Z;
	if (HeightDiff < -MaxStepHeight * StepDownHeightFraction)
	{
		return true;
	}

	const float OldSurfaceFriction = GetFrictionFromHit(OldFloor.HitResult);
	const float SpeedMult          = SpeedMultMax / Velocity.Size2D();
	const bool  bSliding           = OldSurfaceFriction * SpeedMult < 0.5f;

	const float ZDiff        = NewFloor.HitResult.ImpactNormal.Z - OldFloor.HitResult.ImpactNormal.Z;
	const bool  bGainingRamp = ZDiff >= 0.0f;

	const float Slope            = Velocity | OldFloor.HitResult.ImpactNormal;
	const bool  bWasGoingUpRamp  = Slope < 0.0f;

	const float StrafeMovement  = FMath::Abs(GetLastInputVector() | GetOwner()->GetActorRightVector());
	const bool  bStrafingOffRamp = StrafeMovement > 0.0f;

	const bool bMovingForCatchAir = bWasGoingUpRamp || bStrafingOffRamp;
	if (bSliding && bGainingRamp && bMovingForCatchAir)
	{
		return true;
	}
	return Super::ShouldCatchAir(OldFloor, NewFloor);
}

bool UPBPlayerMovement::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	return Super::IsWithinEdgeTolerance(CapsuleLocation, TestImpactPoint, CapsuleRadius);
}

bool UPBPlayerMovement::ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const
{
	return Super::ShouldCheckForValidLandingSpot(DeltaTime, Delta, Hit);
}

void UPBPlayerMovement::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);
	if (TimeSlice > 0.0f && MoveDelta != FVector::ZeroVector && MoveDelta.Z)
	{
		UpdateSurfaceFriction(true);
	}
}

bool UPBPlayerMovement::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const
{
	if (!Super::IsValidLandingSpot(CapsuleLocation, Hit))
	{
		return false;
	}
	// Slope bug fix: if moving up a slope and going too fast, don't land
	if (Hit.Normal.Z < 1.0f && (Velocity | Hit.Normal) < 0.0f)
	{
		FVector DeflectionVector = Velocity;
		DeflectionVector.Z += 0.5f * GetGravityZ() * GetWorld()->GetDeltaSeconds();
		DeflectionVector = ComputeSlideVector(DeflectionVector, 1.0f, Hit.Normal, Hit);
		if (DeflectionVector.Z > JumpVelocity)
		{
			return false;
		}
	}
	return true;
}

// ============================================================
// OnMovementModeChanged
// ============================================================
void UPBPlayerMovement::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	StepSide = false;

	bool bJumped         = false;
	bool bQueueJumpSound = false;

	if (MovementMode == MOVE_None)
	{
		bHasEverLanded = false;
	}

	if (PreviousMovementMode == MOVE_Walking && MovementMode == MOVE_Falling)
	{
		bJumped         = true;
		bQueueJumpSound = Velocity.Z > 0.0f;
	}
	else if (PreviousMovementMode == MOVE_Falling && MovementMode == MOVE_Walking)
	{
		bQueueJumpSound = true;
		if (bDeferCrouchSlideToLand)
		{
			bDeferCrouchSlideToLand = false;
			StartCrouchSlide();
		}
	}

	if (bHasDeferredMovementMode)
	{
		bQueueJumpSound = false;
	}

	bool bDidPlayJumpSound = false;
	if (bQueueJumpSound)
	{
		if (!bHasEverLanded && GetOwner()->GetGameTimeSinceCreation() > 0.1f)
		{
			bHasEverLanded = true;
		}
		if (bHasEverLanded)
		{
			FHitResult Hit;
			TraceCharacterFloor(Hit);
			PlayJumpSound(Hit, bJumped);
			bDidPlayJumpSound = true;
		}
	}

	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

	if (!bDidPlayJumpSound)
	{
		if (MovementMode == MOVE_Walking && (GetMovementBase() || CurrentFloor.bBlockingHit))
		{
			bHasEverLanded = true;
		}
	}
}

// ============================================================
// Camera
// ============================================================
float UPBPlayerMovement::GetCameraRoll()
{
	if (RollSpeed == 0.0f || RollAngle == 0.0f) { return 0.0f; }
	float Side       = Velocity | FRotationMatrix(GetCharacterOwner()->GetControlRotation()).GetScaledAxis(EAxis::Y);
	const float Sign = FMath::Sign(Side);
	Side = FMath::Abs(Side);
	Side = (Side < RollSpeed) ? (Side * RollAngle / RollSpeed) : RollAngle;
	return Side * Sign;
}

// ============================================================
// Ladder / NoClip
// ============================================================
bool UPBPlayerMovement::IsOnLadder()       const { return bOnLadder; }
float UPBPlayerMovement::GetLadderClimbSpeed() const { return LadderSpeed; }

void UPBPlayerMovement::SetNoClip(bool bNoClip)
{
	if (bNoClip)
	{
		SetMovementMode(MOVE_Flying);
		DeferredMovementMode = MOVE_Flying;
		bCheatFlying = true;
		GetCharacterOwner()->SetActorEnableCollision(false);
	}
	else
	{
		SetMovementMode(MOVE_Walking);
		DeferredMovementMode = MOVE_Walking;
		bCheatFlying = false;
		GetCharacterOwner()->SetActorEnableCollision(true);
	}
	bHasDeferredMovementMode = true;
}
void UPBPlayerMovement::ToggleNoClip() { SetNoClip(!bCheatFlying); }

// ============================================================
// ApplyVelocityBraking
// ============================================================
#ifndef DIRECTIONAL_BRAKING
#define DIRECTIONAL_BRAKING 0
#endif

void UPBPlayerMovement::ApplyVelocityBraking(float DeltaTime, float Friction, float BrakingDeceleration)
{
	if (Velocity.IsNearlyZero(0.1f) || !HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	const float Speed         = Velocity.Size2D();
	const float FrictionFactor = FMath::Max(0.0f, BrakingFrictionFactor);
	Friction = FMath::Max(0.0f, Friction * FrictionFactor);

	if (ShouldCrouchSlide())
	{
		if (Friction > 1.0f)
		{
			const float TimeDifference = GetWorld()->GetTimeSeconds() - CrouchSlideStartTime;
			Friction = FMath::Lerp(1.0f, Friction, FMath::Clamp(TimeDifference / CrouchSlideBoostTime, 0.0f, 1.0f));
		}
		BrakingDeceleration = FMath::Max(10.0f, Speed);
	}
	else
	{
		BrakingDeceleration = FMath::Max(BrakingDeceleration, Speed);
	}

	const bool bZeroFriction = FMath::IsNearlyZero(Friction);
	const bool bZeroBraking  = (BrakingDeceleration == 0.0f);
	if (bZeroFriction || bZeroBraking) { return; }

	const FVector OldVel   = Velocity;
	float RemainingTime    = DeltaTime;
	const float MaxTimeStep = FMath::Clamp(BrakingSubStepTime, 1.0f / 75.0f, 1.0f / 20.0f);
	const FVector RevAccel = -Velocity.GetSafeNormal();

	while (RemainingTime >= MIN_TICK_TIME)
	{
		const float Delta = (RemainingTime > MaxTimeStep ? FMath::Min(MaxTimeStep, RemainingTime * 0.5f) : RemainingTime);
		RemainingTime -= Delta;
		Velocity += (Friction * BrakingDeceleration * RevAccel) * Delta;
		if ((Velocity | OldVel) <= 0.0f)
		{
			Velocity = FVector::ZeroVector;
			return;
		}
	}

	// UE4 port: UE_KINDA_SMALL_NUMBER does not exist in UE4; use KINDA_SMALL_NUMBER
	if (Velocity.IsNearlyZero(KINDA_SMALL_NUMBER))
	{
		Velocity = FVector::ZeroVector;
	}
}

bool UPBPlayerMovement::ShouldLimitAirControl(float DeltaTime, const FVector& FallAcceleration) const { return false; }

FVector UPBPlayerMovement::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
	FVector FallVel = Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
	FallVel.Z = FMath::Clamp(FallVel.Z, -AxisSpeedLimit, AxisSpeedLimit);
	return FallVel;
}

// ============================================================
// Character state updates
// ============================================================
void UPBPlayerMovement::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
	Velocity.Z = FMath::Clamp(Velocity.Z, -AxisSpeedLimit, AxisSpeedLimit);
	bSlidingInAir = false;
	UpdateCrouching(DeltaSeconds);
}

void UPBPlayerMovement::UpdateCharacterStateAfterMovement(float DeltaSeconds)
{
	Super::UpdateCharacterStateAfterMovement(DeltaSeconds);
	Velocity.Z = FMath::Clamp(Velocity.Z, -AxisSpeedLimit, AxisSpeedLimit);
	UpdateSurfaceFriction(bSlidingInAir);
	bWasSlidingInAir = bSlidingInAir;
	UpdateCrouching(DeltaSeconds, true);
}

void UPBPlayerMovement::UpdateSurfaceFriction(bool bIsSliding)
{
	if (!IsFalling() && CurrentFloor.IsWalkableFloor())
	{
		bSlidingInAir = false;
		if (OldBase.Get() != CurrentFloor.HitResult.GetComponent() || !CurrentFloor.HitResult.Component.IsValid())
		{
			OldBase = CurrentFloor.HitResult.GetComponent();
			FHitResult Hit;
			TraceCharacterFloor(Hit);
			SurfaceFriction = GetFrictionFromHit(Hit);
		}
	}
	else
	{
		bSlidingInAir = bIsSliding;
		const bool bPlayerControlsMovedVertically = IsOnLadder() || Velocity.Z > JumpVelocity || Velocity.Z <= 0.0f || bCheatFlying;
		if (bPlayerControlsMovedVertically) { SurfaceFriction = 1.0f; }
		else if (bIsSliding)                { SurfaceFriction = 0.25f; }
	}
}

void UPBPlayerMovement::UpdateCrouching(float DeltaTime, bool bOnlyUncrouch)
{
	if (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy) { return; }

	if (bIsInCrouchTransition && !bCheatFlying)
	{
		if ((!bOnlyUncrouch && !bWantsToCrouch) || (bOnlyUncrouch && !CanCrouchInCurrentState()))
		{
			if (!(bLockInCrouch && CharacterOwner->bIsCrouched))
			{
				IsWalking() ? DoUnCrouchResize(UncrouchTime, DeltaTime) : DoUnCrouchResize(UncrouchJumpTime, DeltaTime);
			}
		}
		else if (!bOnlyUncrouch)
		{
			if (IsOnLadder()) { bIsInCrouchTransition = false; }
			else { IsWalking() ? DoCrouchResize(CrouchTime, DeltaTime) : DoCrouchResize(CrouchJumpTime, DeltaTime); }
		}
	}
}

// ============================================================
// Crouch sliding
// ============================================================
void UPBPlayerMovement::StartCrouchSlide()
{
	float CurrentTime = GetWorld()->GetTimeSeconds();
	if (IsCrouchSliding() || CurrentTime - CrouchSlideStartTime <= CrouchSlideCooldown)
	{
		if (Velocity.SizeSquared2D() >= MinCrouchSlideBoost * MinCrouchSlideBoost)
		{
			bCrouchSliding = true;
		}
		return;
	}
	const FVector FloorNormal    = CurrentFloor.HitResult.ImpactNormal;
	const FVector CrouchSlideDir = GetOwner()->GetActorForwardVector();
	float Slope    = (CrouchSlideDir | FloorNormal);
	float NewSpeed = FMath::Max(MinCrouchSlideBoost, Velocity.Size2D() * CrouchSlideBoostMultiplier);
	if (NewSpeed > MinCrouchSlideBoost && Slope < 0.0f)
	{
		NewSpeed = FMath::Clamp(NewSpeed + CrouchSlideBoostSlopeFactor * (NewSpeed - MinCrouchSlideBoost) * Slope, MinCrouchSlideBoost, NewSpeed);
	}
	Velocity = NewSpeed * Velocity.GetSafeNormal2D();
	CrouchSlideStartTime = CurrentTime;
	bCrouchSliding = true;
}

bool UPBPlayerMovement::ShouldCrouchSlide() const { return bCrouchSliding && IsMovingOnGround(); }
void UPBPlayerMovement::StopCrouchSliding()        { bCrouchSliding = false; bDeferCrouchSlideToLand = false; }
void UPBPlayerMovement::ToggleCrouchLock(bool bLock) { bLockInCrouch = bLock; }

// ============================================================
// Floor tracing
// ============================================================
float UPBPlayerMovement::GetFrictionFromHit(const FHitResult& Hit) const
{
	float HitSurfaceFriction = 1.0f;
	if (Hit.PhysMaterial.IsValid())
	{
		HitSurfaceFriction = FMath::Min(1.0f, Hit.PhysMaterial->Friction * 1.25f);
	}
	return HitSurfaceFriction;
}

void UPBPlayerMovement::TraceCharacterFloor(FHitResult& OutHit) const
{
	FCollisionQueryParams CapsuleParams(SCENE_QUERY_STAT(CharacterFloorTrace), false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);
	CapsuleParams.bTraceComplex          = true;
	CapsuleParams.bReturnPhysicalMaterial = true;

	const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel   = UpdatedComponent->GetCollisionObjectType();

	// 3.1.0 fix: subtract capsule half-height so trace starts at the bottom of the capsule
	// (fixes "trace distance too short in some cases" bug from 3.1.0 changelog)
	FVector PawnLocation = UpdatedComponent->GetComponentLocation();
	PawnLocation.Z -= StandingCapsuleShape.GetCapsuleHalfHeight();
	FVector StandingLocation = PawnLocation;
	StandingLocation.Z -= MAX_FLOOR_DIST * 10.0f;

	GetWorld()->SweepSingleByChannel(OutHit, PawnLocation, StandingLocation, FQuat::Identity,
		CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
}

void UPBPlayerMovement::TraceLineToFloor(FHitResult& OutHit) const
{
	FCollisionQueryParams CapsuleParams(SCENE_QUERY_STAT(TraceLineToFloor), false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);

	const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel   = UpdatedComponent->GetCollisionObjectType();

	FVector PawnLocation = UpdatedComponent->GetComponentLocation();
	PawnLocation.Z -= StandingCapsuleShape.GetCapsuleHalfHeight();

	if (Acceleration.IsNearlyZero())
	{
		if (!Velocity.IsNearlyZero()) { PawnLocation += Velocity.GetSafeNormal2D() * EdgeFrictionDist; }
	}
	else
	{
		PawnLocation += Acceleration.GetSafeNormal2D() * EdgeFrictionDist;
	}

	FVector StandingLocation = PawnLocation;
	StandingLocation.Z -= EdgeFrictionHeight;
	GetWorld()->SweepSingleByChannel(OutHit, PawnLocation, StandingLocation, FQuat::Identity,
		CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
}

// ============================================================
// Sound
// ============================================================
void UPBPlayerMovement::PlayMoveSound(const float DeltaTime)
{
	if (!bShouldPlayMoveSounds) { return; }
	if (MoveSoundTime > 0.0f)
	{
		MoveSoundTime = FMath::Max(0.0f, MoveSoundTime - 1000.0f * DeltaTime);
	}
	if (MoveSoundTime > 0.0f) { return; }

	// 3.1.0 fix: use SizeSquared2D() not SizeSquared() (was using 3D speed, wrong)
	const float Speed = Velocity.SizeSquared2D();
	float WalkSpeedThreshold, SprintSpeedThreshold;

	if (IsCrouching() || IsOnLadder())
	{
		WalkSpeedThreshold   = MaxWalkSpeedCrouched;
		SprintSpeedThreshold = MaxWalkSpeedCrouched * 1.7f;
	}
	else
	{
		WalkSpeedThreshold   = WalkSpeed;
		SprintSpeedThreshold = SprintSpeed;
	}

	const bool bPlaySound = (bBrakingFrameTolerated || IsOnLadder()) &&
		Speed >= WalkSpeedThreshold * WalkSpeedThreshold && !ShouldCrouchSlide();
	if (!bPlaySound) { return; }

	const bool bSprinting = (Speed >= SprintSpeedThreshold * SprintSpeedThreshold);
	float MoveSoundVolume  = 0.f;
	UPBMoveStepSound* MoveSound = nullptr;

	if (IsOnLadder())
	{
		MoveSoundVolume = 0.5f;
		MoveSoundTime   = 450.0f;
		MoveSound = GetMoveStepSoundBySurface(SurfaceType1);
	}
	else
	{
		MoveSoundTime = bSprinting ? 300.0f : 400.0f;
		FHitResult Hit;
		TraceCharacterFloor(Hit);
		if (Hit.PhysMaterial.IsValid())
		{
			MoveSound = GetMoveStepSoundBySurface(Hit.PhysMaterial->SurfaceType);
		}
		if (!MoveSound) { MoveSound = GetMoveStepSoundBySurface(SurfaceType_Default); }
		if (MoveSound)
		{
			MoveSoundVolume = bSprinting ? MoveSound->GetSprintVolume() : MoveSound->GetWalkVolume();
			if (IsCrouching()) { MoveSoundVolume *= 0.65f; MoveSoundTime += 100.0f; }
		}
	}

	if (!MoveSound) { return; }

	TArray<USoundCue*> MoveSoundCues;
	if (bSprinting && !IsOnLadder())
	{
		MoveSoundCues = StepSide ? MoveSound->GetSprintLeftSounds() : MoveSound->GetSprintRightSounds();
	}
	if (!bSprinting || IsOnLadder() || MoveSoundCues.Num() < 1)
	{
		MoveSoundCues = StepSide ? MoveSound->GetStepLeftSounds() : MoveSound->GetStepRightSounds();
	}
	if (MoveSoundCues.Num() < 1)
	{
		MoveSound = GetMoveStepSoundBySurface(SurfaceType_Default);
		if (!MoveSound) { return; }
		if (bSprinting) { MoveSoundCues = StepSide ? MoveSound->GetSprintLeftSounds() : MoveSound->GetSprintRightSounds(); }
		if (!bSprinting || MoveSoundCues.Num() < 1) { MoveSoundCues = StepSide ? MoveSound->GetStepLeftSounds() : MoveSound->GetStepRightSounds(); }
		if (MoveSoundCues.Num() < 1) { return; }
	}

	USoundCue* Sound  = MoveSoundCues[MoveSoundCues.Num() == 1 ? 0 : FMath::RandRange(0, MoveSoundCues.Num() - 1)];
	Sound->VolumeMultiplier = MoveSoundVolume;

	const FVector StepRelativeLocation(0.0f, 0.0f, -GetCharacterOwner()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	UGameplayStatics::SpawnSoundAttached(Sound, UpdatedComponent, NAME_None, StepRelativeLocation, FRotator::ZeroRotator);
	StepSide = !StepSide;
}

void UPBPlayerMovement::PlayJumpSound(const FHitResult& Hit, bool bJumped)
{
	if (!bShouldPlayMoveSounds) { return; }

	UPBMoveStepSound* MoveSound     = nullptr;
	TSubclassOf<UPBMoveStepSound>* GotSound = nullptr;
	if (Hit.PhysMaterial.IsValid())
	{
		GotSound = PBPlayerCharacter->GetMoveStepSound(Hit.PhysMaterial->SurfaceType);
	}
	if (GotSound) { MoveSound = GotSound->GetDefaultObject(); }
	if (!MoveSound)
	{
		auto* DefaultSound = PBPlayerCharacter->GetMoveStepSound(TEnumAsByte<EPhysicalSurface>(SurfaceType_Default));
		if (!DefaultSound) { return; }
		MoveSound = DefaultSound->GetDefaultObject();
	}
	if (!MoveSound) { return; }

	float MoveSoundVolume;
	if (!bJumped)
	{
		const float FallSpeed = GetFallSpeed(true);
		if      (FallSpeed > PBPlayerCharacter->GetMinSpeedForFallDamage())        { MoveSoundVolume = 1.0f; }
		else if (FallSpeed > PBPlayerCharacter->GetMinSpeedForFallDamage() / 2.0f) { MoveSoundVolume = 0.85f; }
		else if (FallSpeed < PBPlayerCharacter->GetMinLandBounceSpeed())            { MoveSoundVolume = 0.0f; }
		else                                                                         { MoveSoundVolume = 0.5f; }
	}
	else
	{
		MoveSoundVolume = PBPlayerCharacter->IsSprinting() ? MoveSound->GetSprintVolume() : MoveSound->GetWalkVolume();
	}
	if (IsCrouching()) { MoveSoundVolume *= 0.65f; }
	if (MoveSoundVolume <= 0.0f) { return; }

	const TArray<USoundCue*>& MoveSoundCues = bJumped ? MoveSound->GetJumpSounds() : MoveSound->GetLandSounds();
	if (MoveSoundCues.Num() < 1) { return; }

	USoundCue* Sound = MoveSoundCues[MoveSoundCues.Num() == 1 ? 0 : FMath::RandRange(0, MoveSoundCues.Num() - 1)];
	Sound->VolumeMultiplier = MoveSoundVolume;

	const FVector StepRelativeLocation(0.0f, 0.0f, -GetCharacterOwner()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	UGameplayStatics::SpawnSoundAttached(Sound, UpdatedComponent, NAME_None, StepRelativeLocation, FRotator::ZeroRotator);
}

// ============================================================
// CalcVelocity
// ============================================================
void UPBPlayerMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	if (!HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME ||
		(CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy && !bWasSimulatingRootMotion))
	{
		return;
	}

	Friction = FMath::Max(0.0f, Friction);
	const float MaxAccel = GetMaxAcceleration();
	float MaxSpeed       = GetMaxSpeed();

	if (bForceMaxAccel)
	{
		if (Acceleration.SizeSquared() > SMALL_NUMBER)
			Acceleration = Acceleration.GetSafeNormal() * MaxAccel;
		else
			Acceleration = MaxAccel * (Velocity.SizeSquared() < SMALL_NUMBER ? UpdatedComponent->GetForwardVector() : Velocity.GetSafeNormal());
		AnalogInputModifier = 1.0f;
	}

	MaxSpeed = FMath::Max(MaxSpeed * AnalogInputModifier, GetMinAnalogSpeed());

	const bool bZeroAcceleration = Acceleration.IsNearlyZero();
	const bool bIsGroundMove     = IsMovingOnGround() && bBrakingFrameTolerated;

	if (bIsGroundMove || CVarAlwaysApplyFriction->GetBool())
	{
		const bool bVelocityOverMax = IsExceedingMaxSpeed(MaxSpeed);
		const FVector OldVelocity   = Velocity;

		float ActualBrakingFriction = (bUseSeparateBrakingFriction ? BrakingFriction : Friction) * SurfaceFriction;

		if (bIsGroundMove && EdgeFrictionMultiplier != 1.0f)
		{
			bool bDoEdgeFriction = (!bEdgeFrictionOnlyWhenBraking) ||
				(bEdgeFrictionAlwaysWhenCrouching && IsCrouching()) ||
				bZeroAcceleration;
			if (bDoEdgeFriction)
			{
				FHitResult Hit(ForceInit);
				TraceLineToFloor(Hit);
				if (!Hit.bBlockingHit)
				{
					ActualBrakingFriction *= EdgeFrictionMultiplier;
				}
			}
		}

		ApplyVelocityBraking(DeltaTime, ActualBrakingFriction, BrakingDeceleration);

		if (bVelocityOverMax && Velocity.SizeSquared() < FMath::Square(MaxSpeed) && FVector::DotProduct(Acceleration, OldVelocity) > 0.0f)
		{
			Velocity = OldVelocity.GetSafeNormal() * MaxSpeed;
		}
	}

	if (bFluid)
	{
		Velocity = Velocity * (1.0f - FMath::Min(Friction * DeltaTime, 1.0f));
	}

	Velocity.X = FMath::Clamp(Velocity.X, -AxisSpeedLimit, AxisSpeedLimit);
	Velocity.Y = FMath::Clamp(Velocity.Y, -AxisSpeedLimit, AxisSpeedLimit);

	// NoClip
	if (bCheatFlying)
	{
		StopCrouchSliding();
		if (bZeroAcceleration)
		{
			Velocity = FVector(0.0f);
		}
		else
		{
			auto LookVec         = CharacterOwner->GetControlRotation().Vector();
			auto LookVec2D       = CharacterOwner->GetActorForwardVector();
			LookVec2D.Z = 0.0f;
			auto PerpendicularAccel = (LookVec2D | Acceleration) * LookVec2D;
			auto TangentialAccel    = Acceleration - PerpendicularAccel;
			auto Dir                = Acceleration.CosineAngle2D(LookVec);
			auto NoClipClamp        = PBPlayerCharacter->IsSprinting() ? 2.0f * MaxAcceleration : MaxAcceleration;
			Velocity = (Dir * LookVec * PerpendicularAccel.Size2D() + TangentialAccel).GetClampedToSize(NoClipClamp, NoClipClamp);
		}
	}
	// Ladder
	else if (IsOnLadder())
	{
		StopCrouchSliding();
		Velocity = FVector::ZeroVector;
		// Handle ladder movement here if needed
	}
	// Crouch slide
	else if (ShouldCrouchSlide())
	{
		const FVector FloorNormal    = CurrentFloor.HitResult.ImpactNormal;
		const FVector CrouchSlideDir = GetOwner()->GetActorForwardVector();
		const float TimeDifference   = GetWorld()->GetTimeSeconds() - CrouchSlideStartTime;
		FVector WishAccel = CrouchSlideDir * Velocity.Size2D() *
			FMath::Lerp(MaxCrouchSlideVelocityBoost, MinCrouchSlideVelocityBoost, FMath::Clamp(TimeDifference / CrouchSlideBoostTime, 0.0f, 1.0f));
		WishAccel *= 1.0f + (CrouchSlideDir | FloorNormal);
		Velocity += WishAccel * DeltaTime;
		if (Velocity.IsNearlyZero()) { StopCrouchSliding(); }
	}
	// Normal walk / air move
	else
	{
		if (IsMovingOnGround()) { StopCrouchSliding(); }

		if (!bZeroAcceleration)
		{
			const FVector WishAccel = Acceleration.GetClampedToMaxSize2D(MaxSpeed);
			const FVector AccelDir  = WishAccel.GetSafeNormal2D();
			const float Veer        = Velocity.X * AccelDir.X + Velocity.Y * AccelDir.Y;

			float SpeedCap = 0.0f;
			if (!bIsGroundMove)
			{
				const float ForwardAccel = AccelDir | GetOwner()->GetActorForwardVector();
				SpeedCap = (bWasSlidingInAir && FMath::IsNearlyZero(ForwardAccel)) ? AirSlideSpeedCap : AirSpeedCap;
			}

			const float AddSpeed = (bIsGroundMove ? WishAccel : WishAccel.GetClampedToMaxSize2D(SpeedCap)).Size2D() - Veer;
			if (AddSpeed > 0.0f)
			{
				const float AccMult = bIsGroundMove ? GroundAccelerationMultiplier : AirAccelerationMultiplier;
				FVector CurrentAccel = WishAccel * AccMult * SurfaceFriction * DeltaTime;
				CurrentAccel = CurrentAccel.GetClampedToMaxSize2D(AddSpeed);
				Velocity += CurrentAccel;
			}
		}
	}

	Velocity.X = FMath::Clamp(Velocity.X, -AxisSpeedLimit, AxisSpeedLimit);
	Velocity.Y = FMath::Clamp(Velocity.Y, -AxisSpeedLimit, AxisSpeedLimit);

	// Dynamic step height
	const float SpeedSq = Velocity.SizeSquared2D();
	if (IsOnLadder() || SpeedSq <= MaxWalkSpeedCrouched * MaxWalkSpeedCrouched)
	{
		MaxStepHeight = DefaultStepHeight;
		if (GetWalkableFloorZ() != DefaultWalkableFloorZ) { SetWalkableFloorZ(DefaultWalkableFloorZ); }
	}
	else
	{
		const float Speed         = FMath::Sqrt(SpeedSq);
		float SpeedMultiplier     = FMath::Clamp((Speed - SpeedMultMin) / (SpeedMultMax - SpeedMultMin), 0.0f, 1.0f);
		SpeedMultiplier *= SpeedMultiplier;
		if (!IsFalling()) { SpeedMultiplier = FMath::Max((1.0f - SurfaceFriction) * SpeedMultiplier, 0.0f); }
		MaxStepHeight = FMath::Lerp(DefaultStepHeight, MinStepHeight, SpeedMultiplier);
		const float NewWalkableZ = FMath::Lerp(DefaultWalkableFloorZ, 0.9848f, SpeedMultiplier);
		if (GetWalkableFloorZ() != NewWalkableZ) { SetWalkableFloorZ(NewWalkableZ); }
	}
}

// ============================================================
// Crouch
// ============================================================
void UPBPlayerMovement::Crouch(bool bClientSimulation)
{
	if (bClientSimulation) { Super::Crouch(true); return; }

	if (bShouldCrouchSlide && !bCrouchSliding)
	{
		const float ForwardVelocity = Velocity | GetOwner()->GetActorForwardVector();
		if (ForwardVelocity >= SprintSpeed * CrouchSlideSpeedRequirementMultiplier)
		{
			if (!Acceleration.IsNearlyZero() && IsMovingOnGround())
			{
				StartCrouchSlide();
			}
			else if (IsFalling() && Velocity.Z < 0.0f)
			{
				bDeferCrouchSlideToLand = true;
			}
		}
	}
	bIsInCrouchTransition = true;
}

void UPBPlayerMovement::DoCrouchResize(float TargetTime, float DeltaTime, bool bClientSimulation)
{
	if (!HasValidData() || (!bClientSimulation && !CanCrouchInCurrentState()))
	{
		bIsInCrouchTransition = false;
		return;
	}

	UCapsuleComponent* CharacterCapsule = CharacterOwner->GetCapsuleComponent();
	// UE4 port: CrouchedHalfHeight is a direct float member in UE4 — GetCrouchedHalfHeight() is UE5-only
	if (FMath::IsNearlyEqual(CharacterCapsule->GetUnscaledCapsuleHalfHeight(), CrouchedHalfHeight))
	{
		if (!bClientSimulation) { CharacterOwner->bIsCrouched = true; }
		CharacterOwner->OnStartCrouch(0.0f, 0.0f);
		bIsInCrouchTransition = false;
		return;
	}

	ACharacter* DefaultCharacter      = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
	const float ComponentScale        = CharacterCapsule->GetShapeScale();
	const float OldUnscaledHalfHeight = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float OldUnscaledRadius     = CharacterCapsule->GetUnscaledCapsuleRadius();
	// UE4 port: direct member access
	const float FullCrouchDiff        = OldUnscaledHalfHeight - CrouchedHalfHeight;
	const float CurrentUnscaledHalfHeight = CharacterCapsule->GetUnscaledCapsuleHalfHeight();
	const bool  bInstantCrouch        = FMath::IsNearlyZero(TargetTime);
	const float CurrentAlpha          = 1.0f - (CurrentUnscaledHalfHeight - CrouchedHalfHeight) / FullCrouchDiff;

	float TargetAlphaDiff = 1.0f, TargetAlpha = 1.0f;
	if (!bInstantCrouch)
	{
		TargetAlphaDiff = DeltaTime / CrouchTime;
		TargetAlpha     = CurrentAlpha + TargetAlphaDiff;
	}
	if (TargetAlpha >= 1.0f || FMath::IsNearlyEqual(TargetAlpha, 1.0f))
	{
		TargetAlpha = 1.0f; TargetAlphaDiff = TargetAlpha - CurrentAlpha;
		bIsInCrouchTransition = false; CharacterOwner->bIsCrouched = true;
	}

	const float TargetCrouchedHalfHeight  = OldUnscaledHalfHeight - FullCrouchDiff * TargetAlpha;
	const float ClampedCrouchedHalfHeight = FMath::Max3(0.0f, OldUnscaledRadius, TargetCrouchedHalfHeight);
	CharacterCapsule->SetCapsuleSize(OldUnscaledRadius, ClampedCrouchedHalfHeight);
	const float HalfHeightAdjust       = FullCrouchDiff * TargetAlphaDiff;
	const float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	if (!bClientSimulation)
	{
		if (bCrouchMaintainsBaseLocation)
			UpdatedComponent->MoveComponent(FVector(0, 0, -ScaledHalfHeightAdjust), UpdatedComponent->GetComponentQuat(), true, nullptr, MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
		else
			UpdatedComponent->MoveComponent(FVector(0, 0,  ScaledHalfHeightAdjust), UpdatedComponent->GetComponentQuat(), true, nullptr, MOVECOMP_NoFlags, ETeleportType::None);
	}

	bForceNextFloorCheck = true;
	const float MeshAdjust = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - ClampedCrouchedHalfHeight;
	AdjustProxyCapsuleSize();
	CharacterOwner->OnStartCrouch(MeshAdjust, MeshAdjust * ComponentScale);

	if ((bClientSimulation && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy) ||
		(IsNetMode(NM_ListenServer) && CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy))
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData)
		{
			ClientData->MeshTranslationOffset -= FVector(0, 0, ScaledHalfHeightAdjust);
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
		}
	}
}

void UPBPlayerMovement::UnCrouch(bool bClientSimulation)
{
	if (bClientSimulation) { Super::UnCrouch(true); return; }
	bIsInCrouchTransition = true;
	StopCrouchSliding();
}

void UPBPlayerMovement::DoUnCrouchResize(float TargetTime, float DeltaTime, bool bClientSimulation)
{
	if (!HasValidData()) { bIsInCrouchTransition = false; return; }

	ACharacter* DefaultCharacter   = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
	UCapsuleComponent* CharacterCapsule = CharacterOwner->GetCapsuleComponent();

	// UE4 port: direct CrouchedHalfHeight member access (GetCrouchedHalfHeight() is UE5-only)
	if (FMath::IsNearlyEqual(CharacterCapsule->GetUnscaledCapsuleHalfHeight(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight()))
	{
		if (!bClientSimulation) { CharacterOwner->bIsCrouched = false; }
		CharacterOwner->OnEndCrouch(0.0f, 0.0f);
		bCrouchFrameTolerated = false; bIsInCrouchTransition = false;
		return;
	}

	const float CurrentCrouchedHalfHeight = CharacterCapsule->GetScaledCapsuleHalfHeight();
	const float ComponentScale            = CharacterCapsule->GetShapeScale();
	const float OldUnscaledHalfHeight     = CharacterCapsule->GetUnscaledCapsuleHalfHeight();
	const float UncrouchedHeight          = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	// UE4 port: direct member access
	const float FullCrouchDiff            = UncrouchedHeight - CrouchedHalfHeight;
	const bool  bInstantCrouch            = FMath::IsNearlyZero(TargetTime);
	float CurrentAlpha = 1.0f - (UncrouchedHeight - OldUnscaledHalfHeight) / FullCrouchDiff;
	float TargetAlphaDiff = 1.0f, TargetAlpha = 1.0f;
	const UWorld* MyWorld     = GetWorld();
	const FVector PawnLocation = UpdatedComponent->GetComponentLocation();

	if (!bInstantCrouch)
	{
		TargetAlphaDiff = DeltaTime / TargetTime;
		TargetAlpha     = CurrentAlpha + TargetAlphaDiff;
		if (bCrouchMaintainsBaseLocation)
		{
			const float SweepInflation = KINDA_SMALL_NUMBER * 10.0f;
			FCollisionQueryParams CapsuleParams(SCENE_QUERY_STAT(CrouchTrace), false, CharacterOwner);
			FCollisionResponseParams ResponseParam;
			InitCollisionParams(CapsuleParams, ResponseParam);
			const float HalfHeightAdjust        = ComponentScale * (UncrouchedHeight - OldUnscaledHalfHeight) * GroundUncrouchCheckFactor;
			const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, -SweepInflation - HalfHeightAdjust);
			const ECollisionChannel CollisionChannel   = UpdatedComponent->GetCollisionObjectType();
			FVector StandingLocation = PawnLocation + FVector(0, 0, StandingCapsuleShape.GetCapsuleHalfHeight() - CurrentCrouchedHalfHeight);
			if (MyWorld->OverlapBlockingTestByChannel(StandingLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam))
				return;
		}
	}
	if (TargetAlpha >= 1.0f || FMath::IsNearlyEqual(TargetAlpha, 1.0f))
	{
		TargetAlpha = 1.0f; TargetAlphaDiff = TargetAlpha - CurrentAlpha;
		bIsInCrouchTransition = false; StopCrouchSliding();
	}

	const float HalfHeightAdjust       = FullCrouchDiff * TargetAlphaDiff;
	const float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;
	check(CharacterCapsule);

	if (!bClientSimulation)
	{
		const float SweepInflation = KINDA_SMALL_NUMBER * 10.0f;
		FCollisionQueryParams CapsuleParams(SCENE_QUERY_STAT(CrouchTrace), false, CharacterOwner);
		FCollisionResponseParams ResponseParam;
		InitCollisionParams(CapsuleParams, ResponseParam);
		const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, -SweepInflation - ScaledHalfHeightAdjust);
		const ECollisionChannel CollisionChannel   = UpdatedComponent->GetCollisionObjectType();
		bool bEncroached = true;

		if (!bCrouchMaintainsBaseLocation)
		{
			bEncroached = MyWorld->OverlapBlockingTestByChannel(PawnLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
			if (bEncroached && ScaledHalfHeightAdjust > 0.0f)
			{
				float PawnRadius, PawnHalfHeight;
				CharacterCapsule->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
				const float ShrinkHalfHeight = PawnHalfHeight - PawnRadius;
				const float TraceDist        = PawnHalfHeight - ShrinkHalfHeight;
				FHitResult Hit(1.0f);
				const FCollisionShape ShortCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, ShrinkHalfHeight);
				if (!Hit.bStartPenetrating)
				{
					const float DistanceToBase = (Hit.Time * TraceDist) + ShortCapsuleShape.Capsule.HalfHeight;
					const FVector NewLoc = FVector(PawnLocation.X, PawnLocation.Y,
						PawnLocation.Z - DistanceToBase + StandingCapsuleShape.Capsule.HalfHeight + SweepInflation + MIN_FLOOR_DIST / 2.0f);
					bEncroached = MyWorld->OverlapBlockingTestByChannel(NewLoc, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
					if (!bEncroached)
						UpdatedComponent->MoveComponent(NewLoc - PawnLocation, UpdatedComponent->GetComponentQuat(), false, nullptr, MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
				}
			}
		}
		else
		{
			FVector StandingLocation = PawnLocation + FVector(0, 0, StandingCapsuleShape.GetCapsuleHalfHeight() - CurrentCrouchedHalfHeight);
			bEncroached = MyWorld->OverlapBlockingTestByChannel(StandingLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
			if (bEncroached && IsMovingOnGround())
			{
				const float MinFloorDist = KINDA_SMALL_NUMBER * 10.0f;
				if (CurrentFloor.bBlockingHit && CurrentFloor.FloorDist > MinFloorDist)
				{
					StandingLocation.Z -= CurrentFloor.FloorDist - MinFloorDist;
					bEncroached = MyWorld->OverlapBlockingTestByChannel(StandingLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
				}
			}
			if (!bEncroached)
			{
				UpdatedComponent->MoveComponent(StandingLocation - PawnLocation, UpdatedComponent->GetComponentQuat(), false, nullptr, MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
				bForceNextFloorCheck = true;
			}
		}

		if (bEncroached) { return; }
		CharacterOwner->bIsCrouched = false;
	}
	else
	{
		bShrinkProxyCapsule = true;
	}

	CharacterCapsule->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), OldUnscaledHalfHeight + HalfHeightAdjust, true);
	const float MeshAdjust = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - OldUnscaledHalfHeight + HalfHeightAdjust;
	AdjustProxyCapsuleSize();
	CharacterOwner->OnEndCrouch(MeshAdjust, MeshAdjust * ComponentScale);
	bCrouchFrameTolerated = false;

	if ((bClientSimulation && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy) ||
		(IsNetMode(NM_ListenServer) && CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy))
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData)
		{
			ClientData->MeshTranslationOffset += FVector(0, 0, ScaledHalfHeightAdjust);
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
		}
	}
}

// ============================================================
// MoveUpdatedComponentImpl — 3.1.0 box-sweep approach (more accurate than 2.0.1 line trace)
// ============================================================
bool UPBPlayerMovement::MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& NewRotation, bool bSweep, FHitResult* OutHit, ETeleportType Teleport)
{
	FVector NewDelta = Delta;
	FVector Loc = UpdatedComponent->GetComponentLocation();

	bool bResult = Super::MoveUpdatedComponentImpl(NewDelta, NewRotation, bSweep, OutHit, Teleport);

	if (bSweep && Teleport == ETeleportType::None && Delta != FVector::ZeroVector && IsFalling() && FMath::Abs(Delta.Z) > 0.0f)
	{
		const float HorizontalMovement = Delta.SizeSquared2D();
		// UE4 port: UE_KINDA_SMALL_NUMBER doesn't exist in UE4; use KINDA_SMALL_NUMBER
		if (HorizontalMovement > KINDA_SMALL_NUMBER)
		{
			float PawnRadius, PawnHalfHeight;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
			PawnRadius    *= 0.707f;
			PawnHalfHeight -= SWEEP_EDGE_REJECT_DISTANCE;
			const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(PawnRadius, PawnRadius, PawnHalfHeight));

			FVector Start = Loc;
			Start.Z += Delta.Z;
			FVector DeltaDir = Delta; DeltaDir.Z = 0.0f;
			FVector End = Start + DeltaDir;

			const ECollisionChannel TraceChannel = UpdatedComponent->GetCollisionObjectType();
			FCollisionQueryParams Params(SCENE_QUERY_STAT(CapsuleHemisphereTrace), false, CharacterOwner);
			FCollisionResponseParams ResponseParam;
			InitCollisionParams(Params, ResponseParam);
			FHitResult Hit(1.f);

			// UE4 port: RotateGravityToWorld() is UE5 Custom Gravity API.
			// In UE4, gravity is always downward (-Z), so RotateGravityToWorld(FVector(0,0,-1)) == FVector(0,0,-1).
			// GetWorldToGravityTransform() == FQuat::Identity.
			// UE4 port: UE_PI doesn't exist in UE4 — use PI macro.
			bool bBlockingHit = GetWorld()->SweepSingleByChannel(Hit, Start, End,
				FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);

			if (!bBlockingHit)
			{
				Hit.Reset(1.f, false);
				bBlockingHit = GetWorld()->SweepSingleByChannel(Hit, Start, End,
					FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
			}

			if (bBlockingHit && !Hit.bStartPenetrating && FMath::Abs(Hit.ImpactNormal.Z) <= VERTICAL_SLOPE_NORMAL_Z)
			{
				NewDelta = UMovementComponent::ComputeSlideVector(Delta, 1.0f, Hit.ImpactNormal, Hit);
				if (OutHit) { *OutHit = Hit; }
				FHitResult DiscardHit;
				Super::MoveUpdatedComponentImpl(NewDelta - Delta, NewRotation, bSweep, &DiscardHit, Teleport);
			}
		}
	}
	return bResult;
}

// ============================================================
// CanAttemptJump / GetMaxSpeed
// ============================================================
bool UPBPlayerMovement::CanAttemptJump() const
{
	bool bCanAttemptJump = IsJumpAllowed();
	if (IsMovingOnGround())
	{
		const float FloorZ       = FVector(0, 0, 1) | CurrentFloor.HitResult.ImpactNormal;
		const float WalkableFloor = GetWalkableFloorZ();
		bCanAttemptJump &= (FloorZ >= WalkableFloor) || FMath::IsNearlyEqual(FloorZ, WalkableFloor);
	}
	else if (!IsFalling())
	{
		bCanAttemptJump &= IsOnLadder();
	}
	return bCanAttemptJump;
}

float UPBPlayerMovement::GetMaxSpeed() const
{
	if (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking &&
		MovementMode != MOVE_Falling && MovementMode != MOVE_Flying)
	{
		return Super::GetMaxSpeed();
	}
	if (MovementMode == MOVE_Flying && !IsOnLadder() && !bCheatFlying)
	{
		return Super::GetMaxSpeed();
	}
	if (bCheatFlying)
	{
		return (PBPlayerCharacter->IsSprinting() ? SprintSpeed : WalkSpeed) * 1.5f;
	}
	if (!PBPlayerCharacter->IsSuitEquipped())
	{
		return (IsCrouching() && bCrouchFrameTolerated) ? MaxWalkSpeedCrouched : WalkSpeed;
	}

	float Speed;
	if      (ShouldCrouchSlide())                           { Speed = MinCrouchSlideBoost * MaxCrouchSlideVelocityBoost; }
	else if (IsCrouching() && bCrouchFrameTolerated)        { Speed = MaxWalkSpeedCrouched; }
	else if (PBPlayerCharacter->IsSprinting())              { Speed = SprintSpeed; }
	else if (PBPlayerCharacter->DoesWantToWalk())           { Speed = WalkSpeed; }
	else                                                     { Speed = RunSpeed; }
	return Speed;
}

// ============================================================
// ApplyDownwardForce
// ============================================================
static bool IsSmallBody(const FBodyInstance* Body, float SizeThreshold, float MassThreshold)
{
	if (!Body) { return false; }
	if (Body->GetBodyMass() < MassThreshold) { return true; }
	const FVector Bounds = Body->GetBodyBounds().GetExtent();
	return Bounds.SizeSquared() < SizeThreshold * SizeThreshold;
}

void UPBPlayerMovement::ApplyDownwardForce(float DeltaSeconds)
{
	if (!CurrentFloor.HitResult.IsValidBlockingHit() || StandingDownwardForceScale == 0.0f) { return; }

	UPrimitiveComponent* BaseComp = CurrentFloor.HitResult.GetComponent();
	if (!BaseComp || BaseComp->Mobility != EComponentMobility::Movable) { return; }

	FBodyInstance* BI = BaseComp->GetBodyInstance(CurrentFloor.HitResult.BoneName);
	if (BI && BI->IsInstanceSimulatingPhysics() && !IsSmallBody(BI, 64.0f, 15.0f))
	{
		// UE4 port: GetGravityDirection() is UE5-only.
		// In UE4, gravity always points downward: Gravity = (0, 0, GetGravityZ()).
		// GetGravityZ() returns a negative value, giving the correct downward force.
		const FVector Gravity(0.f, 0.f, GetGravityZ());
		if (!Gravity.IsZero())
		{
			BI->AddForceAtPosition(Gravity * Mass * StandingDownwardForceScale, CurrentFloor.HitResult.ImpactPoint);
		}
	}
}

// ============================================================
// GetMoveStepSoundBySurface
// ============================================================
UPBMoveStepSound* UPBPlayerMovement::GetMoveStepSoundBySurface(EPhysicalSurface SurfaceType) const
{
	TSubclassOf<UPBMoveStepSound>* GotSound = GetPBCharacter()->GetMoveStepSound(TEnumAsByte<EPhysicalSurface>(SurfaceType));
	return GotSound ? GotSound->GetDefaultObject() : nullptr;
}
