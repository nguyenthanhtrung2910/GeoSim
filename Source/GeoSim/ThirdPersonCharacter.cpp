/* Copyright 2022 Esri
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


#include "ThirdPersonCharacter.h"

#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

AThirdPersonCharacter::AThirdPersonCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	GetCharacterMovement()->bOrientRotationToMovement = false;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 0.0f, 360.0f);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 300.0f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
}

void AThirdPersonCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (const APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem
			<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(MappingContext, 0);
		}
	}
}

void AThirdPersonCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	if (const APlayerController* PlayerController = Cast<APlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->RemoveMappingContext(MappingContext);
		}
	}
}

void AThirdPersonCharacter::JumpActionEvent(const FInputActionValue& value)
{
	Jump();
}

void AThirdPersonCharacter::StopJumpActionEvent(const FInputActionValue& value)
{
	StopJumping();
}

void AThirdPersonCharacter::SetCameraBoomSettings() const
{
	if (GetCharacterMovement()->IsFlying())
	{
		CameraBoom->SocketOffset.Z = 75.0f;
	}
	else
	{
		CameraBoom->SocketOffset.Z = 0.0f;
	}
}


void AThirdPersonCharacter::StartFlying(const FInputActionValue& value)
{
	if([[maybe_unused]] auto bInputValue = value.Get<bool>())
	{
		if (!bFlying)
		{
			GetCharacterMovement()->SetMovementMode(MOVE_Flying, 0);
			bFlying = true;
		}
		else if(bFlying)
		{
			GetCharacterMovement()->SetMovementMode(MOVE_Walking, 0);
			bFlying = false;
		}
		SetCameraBoomSettings();
	}
}

void AThirdPersonCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisValue = Value.Get<FVector2D>();
	
	if (GetController())
	{
		AddControllerYawInput(LookAxisValue.X);
		AddControllerPitchInput(-LookAxisValue.Y);
	}
}

void AThirdPersonCharacter::MoveForward(const FInputActionValue& Value)
{
	const auto InputValue = Value.Get<float>();
	const FVector Direction = GetActorForwardVector();
	
	if (GetCharacterMovement()->IsFlying())
	{
		AddMovementInput(Direction, InputValue);
	}
	else
	{
		AddMovementInput(Direction, InputValue);
	}
}

void AThirdPersonCharacter::MoveRight(const FInputActionValue& Value)
{
	const auto InputValue = Value.Get<float>();
	const FVector Direction = GetActorRightVector();
	
	if (GetCharacterMovement()->IsFlying())
	{
		AddMovementInput(Direction, InputValue);
	}
	else
	{
		AddMovementInput(Direction, InputValue);
	}
}

void AThirdPersonCharacter::MoveUp(const FInputActionValue& Value)
{
	const auto InputValue = Value.Get<float>();
	
	if (GetCharacterMovement()->IsFlying())
	{
		AddMovementInput(GetActorUpVector(), InputValue * 10000.0f);
	}
}

void AThirdPersonCharacter::Sprint(const FInputActionValue& Value)
{
	if([[maybe_unused]] auto bInputValue = Value.Get<bool>())
	{
		if(!bSprinting)
		{
			GetCharacterMovement()->MaxWalkSpeed = 2000.0f;
			bSprinting = true;
		}
		else if(bSprinting)
		{
			GetCharacterMovement()->MaxWalkSpeed = 600.0f;
			bSprinting = false;
		}	
	}
}


void AThirdPersonCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	if (UEnhancedInputComponent* EnhancedInputComponent = CastChecked<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AThirdPersonCharacter::Look);
		EnhancedInputComponent->BindAction(MoveForwardAction, ETriggerEvent::Triggered, this, &AThirdPersonCharacter::MoveForward);
		EnhancedInputComponent->BindAction(MoveRightAction, ETriggerEvent::Triggered, this, &AThirdPersonCharacter::MoveRight);
		EnhancedInputComponent->BindAction(MoveUpAction, ETriggerEvent::Triggered, this, &AThirdPersonCharacter::MoveUp);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AThirdPersonCharacter::JumpActionEvent);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AThirdPersonCharacter::StopJumpActionEvent);
		EnhancedInputComponent->BindAction(StartFlyingAction, ETriggerEvent::Started, this, &AThirdPersonCharacter::StartFlying);
		EnhancedInputComponent->BindAction(SprintingAction, ETriggerEvent::Started, this, &AThirdPersonCharacter::Sprint);
	}
}
