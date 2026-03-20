// Copyright Project Borealis
// UE4.27.2 port — see PBPlayerMovement.cpp for full list of changes

#include "Character/PBPlayerCharacter.h"

#include "Runtime/Launch/Resources/Version.h"

// UE4 port: "Engine/DamageEvents.h" was added as a standalone header in UE5.1+
// In UE4.27, FDamageEvent and UDamageType are available via GameFramework/DamageType.h
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Engine/DamageEvents.h"
#endif
#include "GameFramework/DamageType.h"

#include "Components/CapsuleComponent.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

#include "Character/PBPlayerMovement.h"

static TAutoConsoleVariable<int32> CVarAutoBHop(TEXT("move.Pogo"), 1,
	TEXT("If holding spacebar should make the player jump whenever possible.\n"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarJumpBoost(TEXT("move.JumpBoost"), 1,
	TEXT("If the player should boost in a movement direction while jumping.\n"
	     "0 - disables jump boosting entirely\n"
	     "1 - boosts in the direction of input, even when moving in another direction\n"
	     "2 - boosts in the direction of input when moving in the same direction\n"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarBunnyhop(TEXT("move.Bunnyhopping"), 0,
	TEXT("Enable normal bunnyhopping.\n"), ECVF_Default);

const float APBPlayerCharacter::CAPSULE_RADIUS = 30.48f;
const float APBPlayerCharacter::CAPSULE_HEIGHT  = 137.16f;

#ifndef USE_FIRST_PERSON
#define USE_FIRST_PERSON 1
#endif

APBPlayerCharacter::APBPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPBPlayerMovement>(CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

#if USE_FIRST_PERSON
	Mesh1P = ObjectInitializer.CreateDefaultSubobject<USkeletalMeshComponent>(this, TEXT("Mesh1P"));
	Mesh1P->SetupAttachment(GetCapsuleComponent());
	Mesh1P->bOnlyOwnerSee = true;
	Mesh1P->bOwnerNoSee = false;
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Mesh1P->PrimaryComponentTick.TickGroup = TG_PrePhysics;
	Mesh1P->SetCollisionObjectType(ECC_Pawn);
	Mesh1P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh1P->SetCollisionResponseToAllChannels(ECR_Ignore);
#endif

	GetMesh()->bOnlyOwnerSee = false;
	GetMesh()->bOwnerNoSee = true;
	GetMesh()->SetCollisionObjectType(ECC_Pawn);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	GetMesh()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	const float HalfHeight = CAPSULE_HEIGHT / 2.0f;
	GetCapsuleComponent()->InitCapsuleSize(CAPSULE_RADIUS, HalfHeight);

	BaseTurnRate   = 45.0f;
	BaseLookUpRate = 45.0f;

	DefaultBaseEyeHeight = 121.92f - HalfHeight;
	BaseEyeHeight = DefaultBaseEyeHeight;

	constexpr float CrouchedHalfHeight = 68.58f / 2.0f;
	FullCrouchedEyeHeight = 53.34f;
	CrouchedEyeHeight = FullCrouchedEyeHeight - CrouchedHalfHeight;

	MovementPtr = Cast<UPBPlayerMovement>(ACharacter::GetMovementComponent());

	// Fall Damage constants (HL2)
	MinSpeedForFallDamage = 1002.9825f;  // PLAYER_MAX_SAFE_FALL_SPEED
	FatalFallSpeed        = 1757.3625f;  // PLAYER_FATAL_FALL_SPEED
	MinLandBounceSpeed    = 329.565f;    // PLAYER_MIN_BOUNCE_SPEED

	CapDamageMomentumZ = 476.25f;
}

void APBPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();
	MaxJumpTime = -4.0f * GetCharacterMovement()->JumpZVelocity / (3.0f * GetCharacterMovement()->GetGravityZ());
}

void APBPlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bDeferJumpStop)
	{
		bDeferJumpStop = false;
		Super::StopJumping();
	}
}

void APBPlayerCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(APBPlayerCharacter, bIsSprinting, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APBPlayerCharacter, bWantsToWalk, COND_SkipOwner);
}

void APBPlayerCharacter::ApplyDamageMomentum(float DamageTaken, FDamageEvent const& DamageEvent, APawn* PawnInstigator, AActor* DamageCauser)
{
	UDamageType const* const DmgTypeCDO = DamageEvent.DamageTypeClass->GetDefaultObject<UDamageType>();
	if (GetCharacterMovement())
	{
		FVector ImpulseDir;

		if (IsValid(DamageCauser))
		{
			ImpulseDir = (GetActorLocation() - DamageCauser->GetActorLocation()).GetSafeNormal();
		}
		else
		{
			FHitResult HitInfo;
			DamageEvent.GetBestHitInfo(this, DamageCauser, HitInfo, ImpulseDir);
		}

		const float SizeFactor = (60.96f * 60.96f * 137.16f) /
			(FMath::Square(GetCapsuleComponent()->GetScaledCapsuleRadius() * 2.0f) *
			 GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0f);

		float Magnitude = 1.905f * DamageTaken * SizeFactor * 5.0f;
		Magnitude = FMath::Min(Magnitude, 1905.0f);

		FVector Impulse = ImpulseDir * Magnitude;
		const bool bMassIndependentImpulse = !DmgTypeCDO->bScaleMomentumByMass;
		float MassScale = 1.f;
		if (!bMassIndependentImpulse && GetCharacterMovement()->Mass > SMALL_NUMBER)
		{
			MassScale = 1.f / GetCharacterMovement()->Mass;
		}
		if (CapDamageMomentumZ > 0.f)
		{
			Impulse.Z = FMath::Min(Impulse.Z * MassScale, CapDamageMomentumZ) / MassScale;
		}

		GetCharacterMovement()->AddImpulse(Impulse, bMassIndependentImpulse);
	}
}

void APBPlayerCharacter::ClearJumpInput(float DeltaTime)
{
	if (CVarAutoBHop.GetValueOnGameThread() != 0 || bAutoBunnyhop || GetCharacterMovement()->bCheatFlying || bDeferJumpStop)
	{
		return;
	}
	Super::ClearJumpInput(DeltaTime);
}

void APBPlayerCharacter::Jump()
{
	if (GetCharacterMovement()->IsFalling())
	{
		bDeferJumpStop = true;
	}
	Super::Jump();
}

void APBPlayerCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PrevCustomMode)
{
	if (!bPressedJump)
	{
		ResetJumpState();
	}

	if (GetCharacterMovement()->IsFalling())
	{
		if (bProxyIsJumpForceApplied)
		{
			ProxyJumpForceStartedTime = GetWorld()->GetTimeSeconds();
		}
	}
	else
	{
		JumpCurrentCount      = 0;
		JumpKeyHoldTime       = 0.0f;
		JumpForceTimeRemaining = 0.0f;
		// Commented intentionally to allow jumps to retain from falling state (see bDeferJumpStop)
		// bWasJumping = false;
	}

	K2_OnMovementModeChanged(PrevMovementMode, GetCharacterMovement()->MovementMode, PrevCustomMode, GetCharacterMovement()->CustomMovementMode);
	MovementModeChangedDelegate.Broadcast(this, PrevMovementMode, PrevCustomMode);
}

void APBPlayerCharacter::StopJumping()
{
	if (!bDeferJumpStop)
	{
		Super::StopJumping();
	}
}

void APBPlayerCharacter::OnJumped_Implementation()
{
	const int32 JumpBoost = CVarJumpBoost->GetInt();
	if (MovementPtr->IsOnLadder())
	{
		// Implement ladder jump-off here
		return;
	}

	if (GetWorld()->GetTimeSeconds() >= LastJumpBoostTime + MaxJumpTime && JumpBoost)
	{
		LastJumpBoostTime = GetWorld()->GetTimeSeconds();

		const FVector Facing = GetActorForwardVector();
		FVector Input = GetCharacterMovement()->GetCurrentAcceleration();

		if (JumpBoost != 1)
		{
			// Only boost input in the direction of current movement (disables ABH)
			Input *= FMath::IsNearlyZero(Input.GetSafeNormal2D() | GetCharacterMovement()->Velocity.GetSafeNormal2D()) ? 0.0f : 1.0f;
		}

		const float ForwardSpeed   = Input | Facing;
		const float SpeedBoostPerc = bIsSprinting || bIsCrouched ? 0.1f : 0.5f;
		float SpeedAddition        = FMath::Abs(ForwardSpeed * SpeedBoostPerc);
		const float MaxBoostedSpeed = GetCharacterMovement()->GetMaxSpeed() * (1.0f + SpeedBoostPerc);
		const float NewSpeed       = SpeedAddition + GetMovementComponent()->Velocity.Size2D();
		float SpeedAdditionNoClamp = SpeedAddition;

		if (NewSpeed > MaxBoostedSpeed)
		{
			SpeedAddition -= NewSpeed - MaxBoostedSpeed;
		}

		const float AccelMagnitude = GetCharacterMovement()->GetCurrentAcceleration().Size2D();
		if (ForwardSpeed < -AccelMagnitude * FMath::Sin(0.6981f))
		{
			SpeedAddition      *= -1.0f;
			SpeedAdditionNoClamp *= -1.0f;
		}

		FVector JumpBoostedVel    = GetMovementComponent()->Velocity + Facing * SpeedAddition;
		float JumpBoostedSizeSq   = JumpBoostedVel.SizeSquared2D();

		if (CVarBunnyhop.GetValueOnGameThread() != 0)
		{
			FVector JumpBoostedUnclampVel   = GetMovementComponent()->Velocity + Facing * SpeedAdditionNoClamp;
			float JumpBoostedUnclampSizeSq  = JumpBoostedUnclampVel.SizeSquared2D();
			if (JumpBoostedUnclampSizeSq > JumpBoostedSizeSq)
			{
				JumpBoostedVel   = JumpBoostedUnclampVel;
				JumpBoostedSizeSq = JumpBoostedUnclampSizeSq;
			}
		}

		if (GetMovementComponent()->Velocity.SizeSquared2D() < JumpBoostedSizeSq)
		{
			GetMovementComponent()->Velocity = JumpBoostedVel;
		}
	}
}

void APBPlayerCharacter::ToggleNoClip()
{
	MovementPtr->ToggleNoClip();
}

float APBPlayerCharacter::GetFallSpeed(bool bAfterLand)
{
	return MovementPtr->GetFallSpeed(bAfterLand);
}

bool APBPlayerCharacter::CanWalkOn(const FHitResult& Hit) const
{
	return MovementPtr->IsWalkable(Hit);
}

void APBPlayerCharacter::OnCrouch()   { Crouch(); }
void APBPlayerCharacter::OnUnCrouch() { UnCrouch(); }

void APBPlayerCharacter::CrouchToggle()
{
	if (GetCharacterMovement()->bWantsToCrouch) { UnCrouch(); }
	else { Crouch(); }
}

bool APBPlayerCharacter::CanJumpInternal_Implementation() const
{
	bool bCanJump = GetCharacterMovement() && GetCharacterMovement()->IsJumpAllowed();

	if (bCanJump)
	{
		if (!bWasJumping || GetJumpMaxHoldTime() <= 0.0f)
		{
			if (JumpCurrentCount == 0 && GetCharacterMovement()->IsFalling())
			{
				bCanJump = JumpCurrentCount + 1 < JumpMaxCount;
			}
			else
			{
				bCanJump = JumpCurrentCount < JumpMaxCount;
			}
		}
		else
		{
			const bool bJumpKeyHeld = (bPressedJump && JumpKeyHoldTime < GetJumpMaxHoldTime());
			bCanJump = bJumpKeyHeld &&
				(GetCharacterMovement()->IsMovingOnGround() ||
				 (JumpCurrentCount < JumpMaxCount) ||
				 (bWasJumping && JumpCurrentCount == JumpMaxCount));
		}

		if (GetCharacterMovement()->IsMovingOnGround())
		{
			const float FloorZ       = FVector(0.0f, 0.0f, 1.0f) | GetCharacterMovement()->CurrentFloor.HitResult.ImpactNormal;
			const float WalkableFloor = GetCharacterMovement()->GetWalkableFloorZ();
			bCanJump &= (FloorZ >= WalkableFloor || FMath::IsNearlyEqual(FloorZ, WalkableFloor));
		}
	}

	return bCanJump;
}

void APBPlayerCharacter::Turn(float Rate)
{
	AddControllerYawInput(Rate);
}

void APBPlayerCharacter::LookUp(float Rate)
{
	AddControllerPitchInput(Rate);
}

bool APBPlayerCharacter::IsOnLadder() const
{
	// Implement your own ladder code here
	return false;
}

void APBPlayerCharacter::MoveForward(float Val)
{
	if (Val != 0.f)
	{
		const bool bLimitRotation = (GetCharacterMovement()->IsMovingOnGround() || GetCharacterMovement()->IsFalling());
		const FRotator Rotation   = bLimitRotation ? GetActorRotation() : Controller->GetControlRotation();
		const FVector Direction   = FRotationMatrix(Rotation).GetScaledAxis(EAxis::X);
		AddMovementInput(Direction, Val);
	}
}

void APBPlayerCharacter::MoveRight(float Val)
{
	if (Val != 0.f)
	{
		const FQuat Rotation    = GetActorQuat();
		const FVector Direction = FQuatRotationMatrix(Rotation).GetScaledAxis(EAxis::Y);
		AddMovementInput(Direction, Val);
	}
}

void APBPlayerCharacter::MoveUp(float Val)
{
	if (Val != 0.f)
	{
		if (!MovementPtr->bCheatFlying) { return; }
		AddMovementInput(FVector::UpVector, Val);
	}
}

void APBPlayerCharacter::TurnAtRate(float Val)
{
	AddControllerYawInput(Val * BaseTurnRate * GetWorld()->GetDeltaSeconds() / GetActorTimeDilation());
}

void APBPlayerCharacter::LookUpAtRate(float Val)
{
	AddControllerPitchInput(Val * BaseLookUpRate * GetWorld()->GetDeltaSeconds() / GetActorTimeDilation());
}

void APBPlayerCharacter::AddControllerYawInput(float Val)   { Super::AddControllerYawInput(Val); }
void APBPlayerCharacter::AddControllerPitchInput(float Val) { Super::AddControllerPitchInput(Val); }

void APBPlayerCharacter::RecalculateBaseEyeHeight()
{
	const ACharacter* DefaultCharacter    = GetClass()->GetDefaultObject<ACharacter>();
	const float OldUnscaledHalfHeight     = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	// UE4 port: GetCrouchedHalfHeight() is UE5-only; access CrouchedHalfHeight directly
	const float CrouchedHH                = GetCharacterMovement()->CrouchedHalfHeight;
	const float FullCrouchDiff            = OldUnscaledHalfHeight - CrouchedHH;
	const UCapsuleComponent* CharacterCapsule = GetCapsuleComponent();
	const float CurrentUnscaledHalfHeight = CharacterCapsule->GetUnscaledCapsuleHalfHeight();
	const float CurrentAlpha              = 1.0f - (CurrentUnscaledHalfHeight - CrouchedHH) / FullCrouchDiff;
	BaseEyeHeight = FMath::Lerp(DefaultCharacter->BaseEyeHeight, CrouchedEyeHeight, SimpleSpline(CurrentAlpha));
}

bool APBPlayerCharacter::CanCrouch() const
{
	return !GetCharacterMovement()->bCheatFlying && Super::CanCrouch() && !MovementPtr->IsOnLadder();
}

// --- Sample for multiplayer games with a Mesh3P with crouch support ---
#if 0
void APBPlayerCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	const APBPlayerCharacter* DefaultChar = GetDefault<APBPlayerCharacter>(GetClass());
	if (Mesh3P && DefaultChar->Mesh3P)
	{
		FVector MeshRelativeLocation = Mesh3P->GetRelativeLocation();
		MeshRelativeLocation.Z = DefaultChar->Mesh3P->GetRelativeLocation().Z - ScaledHalfHeightAdjust;
		Mesh3P->SetRelativeLocation(MeshRelativeLocation);
	}
}

void APBPlayerCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	const APBPlayerCharacter* DefaultChar = GetDefault<APBPlayerCharacter>(GetClass());
	if (Mesh3P && DefaultChar->Mesh3P)
	{
		FVector MeshRelativeLocation = Mesh3P->GetRelativeLocation();
		MeshRelativeLocation.Z = DefaultChar->Mesh3P->GetRelativeLocation().Z + ScaledHalfHeightAdjust;
		Mesh3P->SetRelativeLocation(MeshRelativeLocation);
	}
}
#endif
